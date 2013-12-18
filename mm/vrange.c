/*
 * mm/vrange.c
 */

#include <linux/vrange.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include "internal.h"
#include <linux/mmu_notifier.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <linux/pagevec.h>
#include <linux/shmem_fs.h>

#define CREATE_TRACE_POINTS
#include <trace/events/vrange.h>

static struct kmem_cache *vrange_cachep;
static struct kmem_cache *vroot_cachep;

static struct vrange_list {
	struct list_head list;
	unsigned long size;
	spinlock_t lock;
} vrange_list;

struct vrange_walker {
	struct vm_area_struct *vma;
	struct list_head *pagelist;
};

#define VRANGE_PURGED_MARK	0

/*
 * [mark|clear]_purge could invalidate cached address but it's rare
 * and at the worst case, some address range would be rescan or skip
 * so it isn't critical for integrity point of view.
 */
void mark_purge(struct vrange *range)
{
	range->hint |= (1 << VRANGE_PURGED_MARK);
}

void clear_purge(struct vrange *range)
{
	range->hint &= ~(1 << VRANGE_PURGED_MARK);
}

bool vrange_purged(struct vrange *range)
{
	bool purged = range->hint & (1 << VRANGE_PURGED_MARK);
	return purged;
}

void record_scan_addr(struct vrange *range, unsigned long addr)
{
	unsigned long old, new, ret;

	BUG_ON(addr & ~PAGE_MASK);

	/*
	 * hint variable is shared by cache address and purged flag.
	 * purged flag is modified while we hold vrange_lock but
	 * cache address is modified without any lock so that it
	 * could invalidate purged flag by racing do_purge, which
	 * is critical. The cmpxchg should prevent it.
	 */
	do {
		old = range->hint;
		new = old | addr;
		ret = cmpxchg(&range->hint, old, new);
	} while (ret != old);

	BUG_ON(addr && addr > range->node.last + 1);
	BUG_ON(addr && addr < range->node.start);
}

unsigned long load_scan_addr(struct vrange *range)
{
	unsigned long cached_addr = range->hint & PAGE_MASK;
	BUG_ON(cached_addr && cached_addr > range->node.last + 1);
	BUG_ON(cached_addr && cached_addr < range->node.start);

	return cached_addr;
}

static inline unsigned int vrange_size(struct vrange *range)
{
	return range->node.last + 1 - range->node.start;
}

static void vroot_ctor(void *data)
{
	struct vrange_root *vroot = data;

	atomic_set(&vroot->refcount, 0);
	mutex_init(&vroot->v_lock);
	vroot->v_rb = RB_ROOT;
}

static int __init vrange_init(void)
{
	INIT_LIST_HEAD(&vrange_list.list);
	spin_lock_init(&vrange_list.lock);
	vroot_cachep = kmem_cache_create("vrange_root",
				sizeof(struct vrange_root), 0,
				SLAB_DESTROY_BY_RCU|SLAB_PANIC, vroot_ctor);
	vrange_cachep = KMEM_CACHE(vrange, SLAB_PANIC);
	return 0;
}
module_init(vrange_init);

static struct vrange_root *__vroot_alloc(gfp_t flags)
{
	struct vrange_root *vroot = kmem_cache_alloc(vroot_cachep, flags);
	if (!vroot)
		return vroot;

	atomic_set(&vroot->refcount, 1);
	return vroot;
}

static inline int __vroot_get(struct vrange_root *vroot)
{
	if (!atomic_inc_not_zero(&vroot->refcount))
		return 0;

	return 1;
}

static inline void __vroot_put(struct vrange_root *vroot)
{
	if (atomic_dec_and_test(&vroot->refcount)) {
		enum {VRANGE_MM, VRANGE_FILE} type = vroot->type;
		if (type == VRANGE_MM) {
			struct mm_struct *mm = vroot->object;
			mmdrop(mm);
		} else if (type == VRANGE_FILE) {
			/* TODO : */
		} else
			BUG();

		WARN_ON(!RB_EMPTY_ROOT(&vroot->v_rb));
		kmem_cache_free(vroot_cachep, vroot);
	}
}

static bool __vroot_init_mm(struct vrange_root *vroot, struct mm_struct *mm)
{
	bool ret = false;

	spin_lock(&mm->page_table_lock);
	if (!mm->vroot) {
		mm->vroot = vroot;
		vrange_root_init(mm->vroot, VRANGE_MM, mm);
		atomic_inc(&mm->mm_count);
		ret = true;
	}
	spin_unlock(&mm->page_table_lock);

	return ret;
}

static bool __vroot_init_mapping(struct vrange_root *vroot,
						struct address_space *mapping)
{
	bool ret = false;

	mutex_lock(&mapping->i_mmap_mutex);
	if (!mapping->vroot) {
		mapping->vroot = vroot;
		vrange_root_init(mapping->vroot, VRANGE_FILE, mapping);
		/* XXX - inc ref count on mapping? */
		ret = true;
	}
	mutex_unlock(&mapping->i_mmap_mutex);

	return ret;
}

static struct vrange_root *vroot_alloc_mm(struct mm_struct *mm)
{
	struct vrange_root *ret, *allocated;

	ret = NULL;
	allocated = __vroot_alloc(GFP_NOFS);
	if (!allocated)
		return NULL;

	if (__vroot_init_mm(allocated, mm)) {
		ret = allocated;
		allocated = NULL;
	}

	if (allocated)
		__vroot_put(allocated);

	return ret;
}

static struct vrange_root *vroot_alloc_vma(struct vm_area_struct *vma)
{
	struct vrange_root *ret, *allocated;
	bool val;

	ret = NULL;
	allocated = __vroot_alloc(GFP_KERNEL);
	if (!allocated)
		return NULL;

	if (vma->vm_file && (vma->vm_flags & VM_SHARED))
		val = __vroot_init_mapping(allocated, vma->vm_file->f_mapping);
	else
		val = __vroot_init_mm(allocated, vma->vm_mm);

	if (val) {
		ret = allocated;
		allocated = NULL;
	}

	if (allocated)
		kmem_cache_free(vroot_cachep, allocated);

	return ret;
}

static struct vrange_root *vrange_get_vroot(struct vrange *vrange)
{
	struct vrange_root *vroot;
	struct vrange_root *ret = NULL;

	rcu_read_lock();
	/*
	 * Prevent compiler from re-fetching vrange->owner while others
	 * clears vrange->owner.
	 */
	vroot = ACCESS_ONCE(vrange->owner);
	if (!vroot)
		goto out;

	/*
	 * vroot couldn't be destroyed while we're holding rcu_read_lock
	 * so it's okay to access vroot
	 */
	if (!__vroot_get(vroot))
		goto out;


	/* If we reach here, vroot is either ours or others because
	 * vroot could be allocated for othres in same RCU period
	 * so we should check it carefully. For free/reallocating
	 * for others, all vranges from vroot->tree should be detached
	 * firstly right before vroot freeing so if we check vrange->owner
	 * isn't NULL, it means vroot is ours.
	 */
	smp_rmb();
	if (!vrange->owner) {
		__vroot_put(vroot);
		goto out;
	}
	ret = vroot;
out:
	rcu_read_unlock();
	return ret;
}

static struct vrange *__vrange_alloc(gfp_t flags)
{
	struct vrange *vrange = kmem_cache_alloc(vrange_cachep, flags);
	if (!vrange)
		return vrange;
	vrange->owner = NULL;
	vrange->hint = 0;
	INIT_LIST_HEAD(&vrange->lru);
	atomic_set(&vrange->refcount, 1);
	return vrange;
}

static void __vrange_free(struct vrange *range)
{
	WARN_ON(range->owner);
	WARN_ON(atomic_read(&range->refcount) != 0);
	WARN_ON(!list_empty(&range->lru));

	kmem_cache_free(vrange_cachep, range);
}

static inline void __vrange_lru_add(struct vrange *range)
{
	spin_lock(&vrange_list.lock);
	/*
	 * We need this check because it could be raced with
	 * shrink_vrange_object and vrange_resize
	 */
	if (list_empty(&range->lru)) {
		list_add(&range->lru, &vrange_list.list);
		vrange_list.size += vrange_size(range);
	}
	spin_unlock(&vrange_list.lock);
}

static inline void __vrange_lru_del(struct vrange *range)
{
	spin_lock(&vrange_list.lock);
	if (!list_empty(&range->lru)) {
		list_del_init(&range->lru);
		vrange_list.size -= vrange_size(range);
		WARN_ON(range->owner);
	}
	spin_unlock(&vrange_list.lock);
}

static void __vrange_add(struct vrange *range, struct vrange_root *vroot)
{
	range->owner = vroot;
	interval_tree_insert(&range->node, &vroot->v_rb);

	WARN_ON(atomic_read(&range->refcount) <= 0);
	__vrange_lru_add(range);
}

static inline int __vrange_get(struct vrange *vrange)
{
	if (!atomic_inc_not_zero(&vrange->refcount))
		return 0;

	return 1;
}

static inline void __vrange_put(struct vrange *range)
{
	if (atomic_dec_and_test(&range->refcount)) {
		__vrange_lru_del(range);
		__vrange_free(range);
	}
}

static void __vrange_remove(struct vrange *range)
{
	interval_tree_remove(&range->node, &range->owner->v_rb);
	range->owner = NULL;
}

static inline void __vrange_set(struct vrange *range,
		unsigned long start_idx, unsigned long end_idx,
		bool purged)
{
	range->node.start = start_idx;
	range->node.last = end_idx;

	/* If resize happens, invalidate cache addr */
	range->hint = 0;
	if (purged)
		mark_purge(range);
	else
		clear_purge(range);
}

static inline void __vrange_resize(struct vrange *range,
		unsigned long start_idx, unsigned long end_idx)
{
	struct vrange_root *vroot = range->owner;
	bool purged = vrange_purged(range);

	__vrange_remove(range);
	__vrange_lru_del(range);
	__vrange_set(range, start_idx, end_idx, purged);
	__vrange_add(range, vroot);
}

static struct vrange *__vrange_find(struct vrange_root *vroot,
					unsigned long start_idx,
					unsigned long end_idx)
{
	struct vrange *range = NULL;
	struct interval_tree_node *node;

	node = interval_tree_iter_first(&vroot->v_rb, start_idx, end_idx);
	if (node)
		range = vrange_from_node(node);
	return range;
}

static int vrange_add(struct vrange_root *vroot,
			unsigned long start_idx, unsigned long end_idx)
{
	struct vrange *new_range, *range;
	struct interval_tree_node *node, *next;
	int purged = 0;

	new_range = __vrange_alloc(GFP_KERNEL);
	if (!new_range)
		return -ENOMEM;

	vrange_lock(vroot);

	node = interval_tree_iter_first(&vroot->v_rb, start_idx, end_idx);
	while (node) {
		next = interval_tree_iter_next(node, start_idx, end_idx);
		range = vrange_from_node(node);
		/* old range covers new range fully */
		if (node->start <= start_idx && node->last >= end_idx) {
			__vrange_put(new_range);
			goto out;
		}

		start_idx = min_t(unsigned long, start_idx, node->start);
		end_idx = max_t(unsigned long, end_idx, node->last);
		purged |= vrange_purged(range);

		__vrange_remove(range);
		__vrange_put(range);

		node = next;
	}

	__vrange_set(new_range, start_idx, end_idx, purged);
	__vrange_add(new_range, vroot);
out:
	vrange_unlock(vroot);
	return 0;
}

static int vrange_remove(struct vrange_root *vroot,
				unsigned long start_idx, unsigned long end_idx,
				int *purged)
{
	struct vrange *new_range, *range;
	struct interval_tree_node *node, *next;
	bool used_new = false;

	if (!vroot)
		return 0;

	if (!purged)
		return -EINVAL;

	*purged = 0;

	new_range = __vrange_alloc(GFP_KERNEL);
	if (!new_range)
		return -ENOMEM;

	vrange_lock(vroot);

	node = interval_tree_iter_first(&vroot->v_rb, start_idx, end_idx);
	while (node) {
		next = interval_tree_iter_next(node, start_idx, end_idx);
		range = vrange_from_node(node);

		*purged |= vrange_purged(range);

		if (start_idx <= node->start && end_idx >= node->last) {
			/* argumented range covers the range fully */
			__vrange_remove(range);
			__vrange_put(range);
		} else if (node->start >= start_idx) {
			/*
			 * Argumented range covers over the left of the
			 * range
			 */
			__vrange_resize(range, end_idx + 1, node->last);
		} else if (node->last <= end_idx) {
			/*
			 * Argumented range covers over the right of the
			 * range
			 */
			__vrange_resize(range, node->start, start_idx - 1);
		} else {
			/*
			 * Argumented range is middle of the range
			 */
			unsigned long last = node->last;
			used_new = true;
			__vrange_resize(range, node->start, start_idx - 1);
			__vrange_set(new_range, end_idx + 1, last,
					vrange_purged(range));
			__vrange_add(new_range, vroot);
			break;
		}

		node = next;
	}
	vrange_unlock(vroot);

	if (!used_new)
		__vrange_put(new_range);

	return 0;
}

int vrange_clear(struct vrange_root *vroot,
					unsigned long start, unsigned long end)
{
	int purged;

	return vrange_remove(vroot, start, end - 1, &purged);
}

void vrange_root_cleanup(struct vrange_root *vroot)
{
	struct vrange *range;
	struct rb_node *node;

	if (vroot == NULL)
		return;

	vrange_lock(vroot);
	/* We should remove node by post-order traversal */
	while ((node = rb_first(&vroot->v_rb))) {
		range = vrange_entry(node);
		__vrange_remove(range);
		__vrange_put(range);
	}
	vrange_unlock(vroot);
	/*
	 * Before removing vroot, we should make sure range-owner
	 * should be NULL. See the smp_rmb of vrange_get_vroot.
	 */
	smp_wmb();
	__vroot_put(vroot);
}

/*
 * It's okay to fail vrange_fork because worst case is child process
 * can't have copied own vrange data structure so that pages in the
 * vrange couldn't be purged. It would be better rather than failing
 * fork.
 * The down_write of both mm->mmap_sem protects mm->vroot race.
 */
int vrange_fork(struct mm_struct *new_mm, struct mm_struct *old_mm)
{
	struct vrange_root *new, *old;
	struct vrange *range, *new_range;
	struct rb_node *next;

	if (!old_mm->vroot)
		return 0;

	new = vroot_alloc_mm(new_mm);
	if (!new)
		return -ENOMEM;

	old = old_mm->vroot;

	vrange_lock(old);
	next = rb_first(&old->v_rb);
	while (next) {
		range = vrange_entry(next);
		next = rb_next(next);
		/*
		 * We can't use GFP_KERNEL because direct reclaim's
		 * purging logic on vrange could be deadlock by
		 * vrange_lock.
		 */
		new_range = __vrange_alloc(GFP_NOIO);
		if (!new_range)
			goto fail;
		__vrange_set(new_range, range->node.start,
				range->node.last, vrange_purged(range));
		__vrange_add(new_range, new);

	}
	vrange_unlock(old);

	return 0;
fail:
	vrange_unlock(old);
	vrange_root_cleanup(new);
	return -ENOMEM;
}

static inline struct vrange_root *__vma_to_vroot(struct vm_area_struct *vma)
{
	struct vrange_root *vroot = NULL;

	if (vma->vm_file && (vma->vm_flags & VM_SHARED))
		vroot = vma->vm_file->f_mapping->vroot;
	else
		vroot = vma->vm_mm->vroot;

	return vroot;
}

static inline struct vrange_root *__vma_to_vroot_get(struct vm_area_struct *vma)
{
	struct vrange_root *vroot = NULL;

	rcu_read_lock();
	vroot = __vma_to_vroot(vma);

	if (!vroot)
		goto out;

	if (!__vroot_get(vroot))
		vroot = NULL;
out:
	rcu_read_unlock();
	return vroot;
}

static inline unsigned long __vma_addr_to_index(struct vm_area_struct *vma,
							unsigned long addr)
{
	if (vma->vm_file && (vma->vm_flags & VM_SHARED))
		return (vma->vm_pgoff << PAGE_SHIFT) + addr - vma->vm_start;
	return addr;
}

static ssize_t do_vrange(struct mm_struct *mm, unsigned long start_idx,
				unsigned long end_idx, int mode, int *purged)
{
	struct vm_area_struct *vma;
	unsigned long orig_start = start_idx;
	ssize_t count = 0, ret = 0;

	down_read(&mm->mmap_sem);

	vma = find_vma(mm, start_idx);
	for (;;) {
		struct vrange_root *vroot;
		unsigned long tmp, vstart_idx, vend_idx;

		if (!vma)
			goto out;

		if (vma->vm_flags & (VM_SPECIAL|VM_LOCKED|VM_MIXEDMAP|
					VM_HUGETLB))
			goto out;

		/* make sure start is at the front of the current vma*/
		if (start_idx < vma->vm_start) {
			start_idx = vma->vm_start;
			if (start_idx > end_idx)
				goto out;
		}

		/* bound tmp to closer of vm_end & end */
		tmp = vma->vm_end - 1;
		if (end_idx < tmp)
			tmp = end_idx;

		vroot = __vma_to_vroot(vma);
		if (!vroot)
			vroot = vroot_alloc_vma(vma);
		if (!vroot)
			goto out;

		vstart_idx = __vma_addr_to_index(vma, start_idx);
		vend_idx = __vma_addr_to_index(vma, tmp);

		/* mark or unmark */
		if (mode == VRANGE_VOLATILE)
			ret = vrange_add(vroot, vstart_idx, vend_idx);
		else if (mode == VRANGE_NONVOLATILE)
			ret = vrange_remove(vroot, vstart_idx, vend_idx,
						purged);

		if (ret)
			goto out;

		/* update count to distance covered so far*/
		count = tmp - orig_start + 1;

		/* move start up to the end of the vma*/
		start_idx = vma->vm_end;
		if (start_idx > end_idx)
			goto out;
		/* move to the next vma */
		vma = vma->vm_next;
	}
out:
	up_read(&mm->mmap_sem);

	/* report bytes successfully marked, even if we're exiting on error */
	if (count)
		return count;

	return ret;
}

/*
 * The vrange(2) system call.
 *
 * Applications can use vrange() to advise the kernel how it should
 * handle paging I/O in this VM area.  The idea is to help the kernel
 * discard pages of vrange instead of swapping out when memory pressure
 * happens. The information provided is advisory only, and can be safely
 * disregarded by the kernel if system has enough free memory.
 *
 * mode values:
 *  VRANGE_VOLATILE - hint to kernel so VM can discard vrange pages when
 *		memory pressure happens.
 *  VRANGE_NONVOLATILE - Removes any volatile hints previous specified in that
 *		range.
 *
 * purged ptr:
 *  Returns 1 if any page in the range being marked nonvolatile has been purged.
 *
 * Return values:
 *  On success vrange returns the number of bytes marked or unmarked.
 *  Similar to write(), it may return fewer bytes then specified if
 *  it ran into a problem.
 *
 *  If an error is returned, no changes were made.
 *
 * Errors:
 *  -EINVAL - start  len < 0, start is not page-aligned, start is greater
 *		than TASK_SIZE or "mode" is not a valid value.
 *  -ENOMEM - Short of free memory in system for successful system call.
 *  -EFAULT - Purged pointer is invalid.
 *  -ENOSUP - Feature not yet supported.
 */
SYSCALL_DEFINE4(vrange, unsigned long, start,
		size_t, len, int, mode, int __user *, purged)
{
	unsigned long end;
	struct mm_struct *mm = current->mm;
	ssize_t ret = -EINVAL;
	int p = 0;

	if (start & ~PAGE_MASK)
		goto out;

	len &= PAGE_MASK;
	if (!len)
		goto out;

	end = start + len;
	if (end < start)
		goto out;

	if (start >= TASK_SIZE)
		goto out;

	if (purged) {
		/* Test pointer is valid before making any changes */
		if (put_user(p, purged))
			return -EFAULT;
	}

	ret = do_vrange(mm, start, end - 1, mode, &p);

	if (purged) {
		if (put_user(p, purged)) {
			/*
			 * This would be bad, since we've modified volatilty
			 * and the change in purged state would be lost.
			 */
			BUG();
		}
	}

out:
	return ret;
}

bool vrange_addr_volatile(struct vm_area_struct *vma, unsigned long addr)
{
	struct vrange_root *vroot;
	struct vrange *vrange;
	unsigned long vstart_idx, vend_idx;
	bool ret = false;

	vroot = __vma_to_vroot_get(vma);

	if (!vroot)
		return ret;

	vstart_idx = __vma_addr_to_index(vma, addr);
	vend_idx = vstart_idx + PAGE_SIZE - 1;

	vrange_lock(vroot);
	vrange = __vrange_find(vroot, vstart_idx, vend_idx);
	if (vrange) {
		/*
		 * vroot can be allocated for another process in
		 * same period so let's check vroot's stability
		 */
		if (likely(vroot == vrange->owner))
			ret = true;
	}
	vrange_unlock(vroot);
	__vroot_put(vroot);

	return ret;
}

bool vrange_addr_purged(struct vm_area_struct *vma, unsigned long addr)
{
	struct vrange_root *vroot;
	struct vrange *range;
	unsigned long vstart_idx;
	bool ret = false;

	vroot = __vma_to_vroot(vma);
	if (!vroot)
		return false;
	vstart_idx = __vma_addr_to_index(vma, addr);

	vrange_lock(vroot);
	range = __vrange_find(vroot, vstart_idx, vstart_idx);
	if (range && vrange_purged(range))
		ret = true;
	vrange_unlock(vroot);
	return ret;
}

/* Caller should hold vrange_lock */
static void do_purge(struct vrange_root *vroot,
		unsigned long start_idx, unsigned long end_idx)
{
	struct vrange *range;
	struct interval_tree_node *node;

	node = interval_tree_iter_first(&vroot->v_rb, start_idx, end_idx);
	while (node) {
		range = container_of(node, struct vrange, node);
		mark_purge(range);
		node = interval_tree_iter_next(node, start_idx, end_idx);
	}
}

static void try_to_discard_one(struct vrange_root *vroot, struct page *page,
				struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;

	VM_BUG_ON(!vroot);
	VM_BUG_ON(!PageLocked(page));

	pte = page_check_address(page, mm, addr, &ptl, 0);
	if (!pte)
		return;

	BUG_ON(vma->vm_flags & (VM_SPECIAL|VM_LOCKED|VM_MIXEDMAP|VM_HUGETLB));

	flush_cache_page(vma, addr, page_to_pfn(page));
	pteval = ptep_clear_flush(vma, addr, pte);

	update_hiwater_rss(mm);
	if (PageAnon(page))
		dec_mm_counter(mm, MM_ANONPAGES);
	else
		dec_mm_counter(mm, MM_FILEPAGES);

	page_remove_rmap(page);
	page_cache_release(page);

	set_pte_at(mm, addr, pte, swp_entry_to_pte(make_vrange_entry()));
	pte_unmap_unlock(pte, ptl);
	mmu_notifier_invalidate_page(mm, addr);

	addr = __vma_addr_to_index(vma, addr);

	do_purge(vroot, addr, addr + PAGE_SIZE - 1);
}

static int try_to_discard_anon_vpage(struct page *page)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	pgoff_t pgoff;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	struct vrange_root *vroot;

	unsigned long address;

	anon_vma = page_lock_anon_vma_read(page);
	if (!anon_vma)
		return -1;

	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	/*
	 * During interating the loop, some processes could see a page as
	 * purged while others could see a page as not-purged because we have
	 * no global lock between parent and child for protecting vrange system
	 * call during this loop. But it's not a problem because the page is
	 * not *SHARED* page but *COW* page so parent and child can see other
	 * data anytime. The worst case by this race is a page was purged
	 * but couldn't be discarded so it makes unnecessary page fault but
	 * it wouldn't be severe.
	 */
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		vma = avc->vma;
		mm = vma->vm_mm;
		vroot = __vma_to_vroot(vma);
		if (!vroot)
			continue;

		address = vma_address(page, vma);
		vrange_lock(vroot);
		if (!__vrange_find(vroot, address, address + PAGE_SIZE - 1)) {
			vrange_unlock(vroot);
			continue;
		}

		try_to_discard_one(vroot, page, vma, address);
		vrange_unlock(vroot);
	}

	page_unlock_anon_vma_read(anon_vma);
	return 0;
}

static int try_to_discard_file_vpage(struct page *page)
{
	struct address_space *mapping = page->mapping;
	pgoff_t pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	struct vm_area_struct *vma;
	struct vrange_root *vroot;
	unsigned long vstart_idx;
	int ret = 1;

	if (!page->mapping)
		return ret;

	vroot = mapping->vroot;
	if (!vroot)
		return ret;

	vstart_idx = page->index << PAGE_SHIFT;

	mutex_lock(&mapping->i_mmap_mutex);
	vrange_lock(vroot);

	if (!__vrange_find(vroot, vstart_idx, vstart_idx + PAGE_SIZE - 1))
		goto out;

	vma_interval_tree_foreach(vma, &mapping->i_mmap, pgoff, pgoff) {
		unsigned long address = vma_address(page, vma);
		try_to_discard_one(vroot, page, vma, address);
	}

	VM_BUG_ON(page_mapped(page));
	shmem_purge_page(mapping->host, page);
	ret = 0;
out:
	vrange_unlock(vroot);
	mutex_unlock(&mapping->i_mmap_mutex);
	return ret;
}

static int try_to_discard_vpage(struct page *page)
{
	if (PageAnon(page))
		return try_to_discard_anon_vpage(page);
	return try_to_discard_file_vpage(page);
}

int discard_vpage(struct page *page)
{
	VM_BUG_ON(!PageLocked(page));
	VM_BUG_ON(PageLRU(page));

	if (!try_to_discard_vpage(page)) {
		if (PageSwapCache(page))
			try_to_free_swap(page);

		if (page_freeze_refs(page, 1)) {
			unlock_page(page);
			if (current_is_kswapd())
				count_vm_event(PGDISCARD_KSWAPD);
			else
				count_vm_event(PGDISCARD_DIRECT);
			return 0;
		}
	}

	return 1;
}

static struct vrange *vrange_isolate(void)
{
	struct vrange *vrange = NULL;
	spin_lock(&vrange_list.lock);
	while (!list_empty(&vrange_list.list)) {
		vrange = list_entry(vrange_list.list.prev,
				struct vrange, lru);
		list_del_init(&vrange->lru);
		vrange_list.size -= vrange_size(vrange);

		/* vrange is going to destroy */
		if (__vrange_get(vrange))
			break;

		vrange = NULL;
	}

	spin_unlock(&vrange_list.lock);
	return vrange;
}

static unsigned int discard_vrange_pagelist(struct list_head *page_list)
{
	struct page *page;
	unsigned int nr_discard = 0;
	LIST_HEAD(ret_pages);
	LIST_HEAD(free_pages);

	while (!list_empty(page_list)) {
		int err;
		page = list_entry(page_list->prev, struct page, lru);
		list_del(&page->lru);
		if (!trylock_page(page)) {
			list_add(&page->lru, &ret_pages);
			continue;
		}

		/*
		 * discard_vpage returns unlocked page if it
		 * is successful
		 */
		err = discard_vpage(page);
		if (err) {
			unlock_page(page);
			list_add(&page->lru, &ret_pages);
			continue;
		}

		ClearPageActive(page);
		list_add(&page->lru, &free_pages);
		dec_zone_page_state(page, NR_ISOLATED_ANON);
		nr_discard++;
	}

	free_hot_cold_page_list(&free_pages, 1);
	list_splice(&ret_pages, page_list);
	return nr_discard;
}

static void vrange_pte_entry(pte_t pteval, unsigned long address,
		unsigned ptent_size, struct mm_walk *walk)
{
	struct page *page;
	struct vrange_walker *vw = walk->private;
	struct vm_area_struct *vma = vw->vma;
	struct list_head *pagelist = vw->pagelist;

	if (pte_none(pteval))
		return;

	if (!pte_present(pteval))
		return;

	page = vm_normal_page(vma, address, pteval);
	if (unlikely(!page))
		return;

	if (!PageLRU(page) || PageLocked(page))
		return;

	/* TODO : Support THP */
	if (unlikely(PageCompound(page)))
		return;

	if (isolate_lru_page(page))
		return;

	list_add(&page->lru, pagelist);

	VM_BUG_ON(page_is_file_cache(page));
	inc_zone_page_state(page, NR_ISOLATED_ANON);
}

static int vrange_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
		struct mm_walk *walk)
{
	pte_t *pte;
	spinlock_t *ptl;

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	for (; addr != end; pte++, addr += PAGE_SIZE)
		vrange_pte_entry(*pte, addr, PAGE_SIZE, walk);
	pte_unmap_unlock(pte - 1, ptl);
	cond_resched();

	return 0;
}

static unsigned int discard_vma_pages(struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long start,
		unsigned long end)
{
	unsigned int ret = 0;
	LIST_HEAD(pagelist);
	struct vrange_walker vw;
	struct mm_walk vrange_walk = {
		.pmd_entry = vrange_pte_range,
		.mm = vma->vm_mm,
		.private = &vw,
	};

	vw.pagelist = &pagelist;
	vw.vma = vma;

	walk_page_range(start, end, &vrange_walk);

	if (!list_empty(&pagelist))
		ret = discard_vrange_pagelist(&pagelist);

	putback_lru_pages(&pagelist);
	return ret;
}

/*
 * vrange->owner isn't stable because caller doesn't hold vrange_lock
 * so avoid touching vrange->owner.
 */
static int __discard_vrange_anon(struct mm_struct *mm, struct vrange *vrange,
				unsigned int *ret_discard, unsigned long *scan)
{
	struct vm_area_struct *vma;
	unsigned int nr_discard = 0;
	unsigned long start = vrange->node.start;
	unsigned long end = vrange->node.last + 1;
	unsigned long cached_addr;
	int ret = 0;

	/* It prevent to destroy vma when the process exist */
	if (!atomic_inc_not_zero(&mm->mm_users))
		return ret;

	if (!down_read_trylock(&mm->mmap_sem)) {
		mmput(mm);
		ret = -EBUSY;
		goto out; /* this vrange could be retried */
	}

	cached_addr = load_scan_addr(vrange);
	if (cached_addr)
		start = cached_addr;

	vma = find_vma(mm, start);
	if (!vma || (vma->vm_start >= end))
		goto out_unlock;

	for (; vma; vma = vma->vm_next) {
		if (vma->vm_start >= end)
			break;
		BUG_ON(vma->vm_flags & (VM_SPECIAL|VM_LOCKED|VM_MIXEDMAP|
					VM_HUGETLB));
		cond_resched();

		start = max(start, vma->vm_start);
		end = min(end, vma->vm_end);
		end = min(start + *scan, end);

		nr_discard += discard_vma_pages(mm, vma, start, end);
		*scan -= (end - start);
		if (!*scan)
			break;
	}

	record_scan_addr(vrange, end);
out_unlock:
	up_read(&mm->mmap_sem);
	mmput(mm);
	*ret_discard = nr_discard;
out:
	return ret;
}

static int __discard_vrange_file(struct address_space *mapping,
				struct vrange *vrange,
				unsigned int *ret_discard, unsigned long *scan)
{
	struct pagevec pvec;
	pgoff_t index;
	int i;
	unsigned long cached_addr;
	unsigned int nr_discard = 0;
	unsigned long start_idx = vrange->node.start;
	unsigned long end_idx = vrange->node.last;
	const pgoff_t start = start_idx >> PAGE_CACHE_SHIFT;
	pgoff_t end;
	LIST_HEAD(pagelist);

	cached_addr = load_scan_addr(vrange);
	if (cached_addr)
		start_idx = cached_addr;

	end_idx = min(start_idx + *scan, end_idx);
	end = end_idx >> PAGE_CACHE_SHIFT;

	pagevec_init(&pvec, 0);
	index = start;
	while (index <= end && pagevec_lookup(&pvec, mapping, index,
			min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1)) {
		for (i = 0; i < pagevec_count(&pvec); i++) {
			struct page *page = pvec.pages[i];
			index = page->index;
			if (index > end)
				break;
			if (isolate_lru_page(page))
				continue;
			list_add(&page->lru, &pagelist);
			inc_zone_page_state(page, NR_ISOLATED_ANON);
		}
		pagevec_release(&pvec);
		cond_resched();
		index++;
	}

	if (!list_empty(&pagelist))
		nr_discard = discard_vrange_pagelist(&pagelist);

	record_scan_addr(vrange, end_idx + 1);
	*ret_discard = nr_discard;
	putback_lru_pages(&pagelist);

	return 0;
}

static int discard_vrange(struct vrange *vrange, unsigned long *scan)
{
	int ret = 0;
	struct vrange_root *vroot;
	unsigned int nr_discard = 0;
	vroot = vrange_get_vroot(vrange);
	if (!vroot)
		return 0;

	/*
	 * Race of vrange->owner could happens with __vrange_remove
	 * but it's okay because subfunctions will check it again
	 */
	if (vrange->owner == NULL)
		goto out;

	if (vroot->type == VRANGE_MM) {
		struct mm_struct *mm = vroot->object;
		ret = __discard_vrange_anon(mm, vrange,
						&nr_discard, scan);
	} else if (vroot->type == VRANGE_FILE) {
		struct address_space *mapping = vroot->object;
		ret = __discard_vrange_file(mapping, vrange,
						&nr_discard, scan);
	}

	if (!ret) {
		if (current_is_kswapd())
			count_vm_events(PGDISCARD_KSWAPD, nr_discard);
		else
			count_vm_events(PGDISCARD_DIRECT, nr_discard);
	}
out:
	__vroot_put(vroot);
	trace_discard_vrange(vrange, nr_discard);
	return nr_discard;
}

#define VRANGE_SCAN_THRESHOLD	(4 << 20)

unsigned long shrink_vrange(enum lru_list lru, struct lruvec *lruvec,
						struct scan_control *sc)
{
	struct vrange *range;
	unsigned long nr_to_reclaim, nr_reclaimed, total_reclaimed = 1;
	unsigned long long list_size;
	unsigned long remained_scan = VRANGE_SCAN_THRESHOLD;

	if (!(sc->gfp_mask & __GFP_IO))
		return 0;

	/*
	 * In current implementation, VM discard volatile pages by
	 * following preference.
	 *
	 * stream pages -> volatile pages -> anon pages
	 *
	 * If we have trouble(ie, DEF_PRIORITY - 2) with reclaiming cache
	 * pages, it means remained cache pages is likely being working set
	 * so it would be better to discard volatile pages rather than
	 * evicting working set.
	 */
	if (lru != LRU_INACTIVE_ANON && lru != LRU_ACTIVE_ANON &&
	    sc->priority >= DEF_PRIORITY - 2)
		return 0;

	nr_to_reclaim = sc->nr_to_reclaim;

	while (nr_to_reclaim > 0 && remained_scan > 0) {
		nr_reclaimed = 0;
		list_size = vrange_list.size;

		/* If list size is too small, stop scanning */
		if (list_size < SWAP_CLUSTER_MAX)
			break;

		range = vrange_isolate();
		/* If there is no more vrange, stop */
		if (!range)
			return total_reclaimed;

		/* range is removing */
		if (!range->owner) {
			__vrange_put(range);
			continue;
		}

		nr_reclaimed = discard_vrange(range, &remained_scan);
		/* If it's EBUSY, retry it after a little */
		if (nr_reclaimed < 0) {
			__vrange_lru_add(range);
			__vrange_put(range);
			continue;
		}

		__vrange_put(range);

		total_reclaimed += nr_reclaimed;
		if (total_reclaimed >= nr_to_reclaim)
			break;
	}

	trace_shrink_vrange(lruvec, sc, total_reclaimed);
	return total_reclaimed;
}
