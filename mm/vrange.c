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

static struct kmem_cache *vrange_cachep;

static struct vrange_list {
	struct list_head list;
	unsigned long size;
	struct mutex lock;
} vrange_list;

static inline unsigned int vrange_size(struct vrange *range)
{
	return range->node.last + 1 - range->node.start;
}

void __init vrange_init(void)
{
	INIT_LIST_HEAD(&vrange_list.list);
	mutex_init(&vrange_list.lock);

	vrange_cachep = KMEM_CACHE(vrange, SLAB_PANIC);
}

static struct vrange *__vrange_alloc(gfp_t flags)
{
	struct vrange *vrange = kmem_cache_alloc(vrange_cachep, flags);
	if (!vrange)
		return vrange;
	vrange->owner = NULL;
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
	mutex_lock(&vrange_list.lock);
	WARN_ON(!list_empty(&range->lru));
	list_add(&range->lru, &vrange_list.list);
	vrange_list.size += vrange_size(range);
	mutex_unlock(&vrange_list.lock);
}

static inline void __vrange_lru_del(struct vrange *range)
{
	mutex_lock(&vrange_list.lock);
	if (!list_empty(&range->lru)) {
		list_del_init(&range->lru);
		vrange_list.size -= vrange_size(range);
		WARN_ON(range->owner);
	}
	mutex_unlock(&vrange_list.lock);
}

static void __vrange_add(struct vrange *range, struct vrange_root *vroot)
{
	range->owner = vroot;
	interval_tree_insert(&range->node, &vroot->v_rb);

	WARN_ON(atomic_read(&range->refcount) <= 0);
	__vrange_lru_add(range);
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
	range->purged = purged;
}

static inline void __vrange_resize(struct vrange *range,
		unsigned long start_idx, unsigned long end_idx)
{
	struct vrange_root *vroot = range->owner;
	bool purged = range->purged;

	__vrange_remove(range);
	__vrange_lru_del(range);
	__vrange_set(range, start_idx, end_idx, purged);
	__vrange_add(range, vroot);
}

static int vrange_add(struct vrange_root *vroot,
			unsigned long start_idx, unsigned long end_idx,
			struct vm_area_struct *vma)
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
		purged |= range->purged;

		__vrange_remove(range);
		__vrange_put(range);

		node = next;
	}

	__vrange_set(new_range, start_idx, end_idx, purged);
	__vrange_add(new_range, vroot);
	vma->is_vrange = 1;
out:
	vrange_unlock(vroot);
	return 0;
}

static int vrange_remove(struct vrange_root *vroot,
				unsigned long start_idx, unsigned long end_idx,
				struct vm_area_struct *vma,
				int *purged)
{
	struct vrange *new_range, *range;
	struct interval_tree_node *node, *next;
	bool used_new = false;

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

		*purged |= range->purged;

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
			used_new = true;
			__vrange_resize(range, node->start, start_idx - 1);
			__vrange_set(new_range, end_idx + 1, node->last,
					range->purged);
			__vrange_add(new_range, vroot);
			break;
		}

		node = next;
	}
	if (vma) {
		if ((vma->vm_start <= start_idx) &&
			((vma->vm_end - 1) <= end_idx))
			vma->is_vrange = 0;
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

	return vrange_remove(vroot, start, end - 1, NULL, &purged);
}

void vrange_root_cleanup(struct vrange_root *vroot)
{
	struct vrange *range;
	struct rb_node *next;

	vrange_lock(vroot);
	next = rb_first(&vroot->v_rb);
	while (next) {
		range = vrange_entry(next);
		next = rb_next(next);
		__vrange_remove(range);
		__vrange_put(range);
	}
	vrange_unlock(vroot);
}

/*
 * It's okay to fail vrange_fork because worst case is child process
 * can't have copied own vrange data structure so that pages in the
 * vrange couldn't be purged. It would be better rather than failing
 * fork.
 */
int vrange_fork(struct mm_struct *new_mm, struct mm_struct *old_mm)
{
	struct vrange_root *new, *old;
	struct vrange *range, *new_range;
	struct rb_node *next;

	new = &new_mm->vroot;
	old = &old_mm->vroot;

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
					range->node.last, range->purged);
		__vrange_add(new_range, new);

	}
	vrange_unlock(old);
	return 0;
fail:
	vrange_root_cleanup(new);
	return -ENOMEM;
}

bool is_vrange(struct mm_struct *mm,
				unsigned long start, unsigned long end)
{
	bool ret;
	struct vrange_root *vroot;
	struct interval_tree_node *node;

	vroot = &mm->vroot;

	vrange_lock(vroot);
	node = interval_tree_iter_first(&vroot->v_rb, start, end - 1);
	vrange_unlock(vroot);
	ret = node ? true : false;
	return ret;
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

		/* Not support vrange-file yet */
		if (!vma || vma->vm_file)
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

		vroot = &mm->vroot;
		vstart_idx = start_idx;
		vend_idx = tmp;

		/* mark or unmark */
		if (mode == VRANGE_VOLATILE)
			ret = vrange_add(vroot, vstart_idx, vend_idx, vma);
		else if (mode == VRANGE_NONVOLATILE)
			ret = vrange_remove(vroot, vstart_idx, vend_idx,
						vma, purged);
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

static bool __within_vrange(struct vrange_root *vroot,
			unsigned long start_idx, unsigned long end_idx)
{
	struct interval_tree_node *node;

	node = interval_tree_iter_first(&vroot->v_rb, start_idx, end_idx);
	return node ? true : false;
}

bool within_vrange(struct vm_area_struct *vma,
			unsigned long start, unsigned long end)
{
	struct vrange_root *vroot;
	unsigned long vstart_idx, vend_idx;
	bool ret;

	vroot = &vma->vm_mm->vroot;
	vstart_idx = start;
	vend_idx = end - 1;

	vrange_lock(vroot);
	ret = __within_vrange(vroot, vstart_idx, vend_idx);
	vrange_unlock(vroot);
	return ret;
}

/* Caller should hold vrange_lock */
static void do_purge(struct vrange_root *vroot,
		unsigned long start, unsigned long end)
{
	struct vrange *range;
	struct interval_tree_node *node;

	node = interval_tree_iter_first(&vroot->v_rb, start, end);
	while (node) {
		range = container_of(node, struct vrange, node);
		range->purged = true;
		node = interval_tree_iter_next(node, start, end);
	}
}

void try_to_discard_one(struct vrange_root *vroot, struct page *page,
			struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;

	VM_BUG_ON(!PageLocked(page));

	pte = page_check_address(page, mm, addr, &ptl, 0);
	if (!pte)
		return;

	BUG_ON(vma->vm_flags & (VM_SPECIAL|VM_LOCKED|VM_MIXEDMAP|VM_HUGETLB));

	flush_cache_page(vma, address, page_to_pfn(page));
	pteval = ptep_clear_flush(vma, addr, pte);

	update_hiwater_rss(mm);
	dec_mm_counter(mm, MM_ANONPAGES);

	page_remove_rmap(page);
	page_cache_release(page);

	set_pte_at(mm, addr, pte, swp_entry_to_pte(make_vrange_entry()));
	pte_unmap_unlock(pte, ptl);
	mmu_notifier_invalidate_page(mm, addr);

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
	bool ret = 1;

	anon_vma = page_lock_anon_vma_read(page);
	if (!anon_vma)
		return ret;

	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		vma = avc->vma;
		mm = vma->vm_mm;
		vroot = &mm->vroot;
		address = vma_address(page, vma);

		vrange_lock(vroot);
		if (!__within_vrange(vroot, address, address + PAGE_SIZE - 1)) {
			vrange_unlock(vroot);
			continue;
		}

		try_to_discard_one(vroot, page, vma, address);
		vrange_unlock(vroot);
	}

	ret = 0;
	page_unlock_anon_vma_read(anon_vma);
	return ret;
}

static int try_to_discard_vpage(struct page *page)
{
	return try_to_discard_anon_vpage(page);
}

int discard_vpage(struct page *page)
{
	VM_BUG_ON(!PageAnon(page));
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

bool purged_vrange(struct vm_area_struct *vma, unsigned long addr)
{
	struct vrange_root *vroot;
	struct interval_tree_node *node;
	struct vrange *range;
	bool ret = false;

	vroot = &vma->vm_mm->vroot;

	vrange_lock(vroot);
	node = interval_tree_iter_first(&vroot->v_rb, addr,
						addr + PAGE_SIZE - 1);
	if (node) {
		range = vrange_from_node(node);
		if (range->purged)
			ret = true;
	}
	vrange_unlock(vroot);
	return ret;
}
