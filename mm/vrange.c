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
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/migrate.h>

struct vrange_walker_private {
	struct zone *zone;
	struct vm_area_struct *vma;
	struct list_head *pagelist;
};

static LIST_HEAD(lru_vrange);
static DEFINE_SPINLOCK(lru_lock);

static struct kmem_cache *vrange_cachep;

static void vrange_ctor(void *data)
{
	struct vrange *vrange = data;
	INIT_LIST_HEAD(&vrange->lru);
}

void __init vrange_init(void)
{
	vrange_cachep = kmem_cache_create("vrange", sizeof(struct vrange),
				0, SLAB_PANIC, vrange_ctor);
}

static inline void __set_vrange(struct vrange *range,
		unsigned long start_idx, unsigned long end_idx)
{
	range->node.start = start_idx;
	range->node.last = end_idx;
}

void lru_add_vrange(struct vrange *vrange)
{
	spin_lock(&lru_lock);
	WARN_ON(!list_empty(&vrange->lru));
	list_add(&vrange->lru, &lru_vrange);
	spin_unlock(&lru_lock);
}

void lru_remove_vrange(struct vrange *vrange)
{
	spin_lock(&lru_lock);
	if (!list_empty(&vrange->lru))
		list_del_init(&vrange->lru);
	spin_unlock(&lru_lock);
}

void lru_move_vrange_to_head(struct mm_struct *mm, unsigned long address)
{
	struct rb_root *root = &mm->v_rb;
	struct interval_tree_node *node;
	struct vrange *vrange;

	vrange_lock(mm);
	node = interval_tree_iter_first(root, address, address + PAGE_SIZE - 1);
	if (node) {
		vrange = container_of(node, struct vrange, node);
		spin_lock(&lru_lock);
		/*
		 * Race happens with get_victim_vrange so in such case,
		 * we can't move but it can put the vrange into head
		 * after finishing purging work so no problem.
		 */
		if (!list_empty(&vrange->lru))
			list_move(&vrange->lru, &lru_vrange);
		spin_unlock(&lru_lock);
	}
	vrange_unlock(mm);
}

static void __add_range(struct vrange *range,
			struct rb_root *root, struct mm_struct *mm)
{
	range->mm = mm;
	lru_add_vrange(range);
	interval_tree_insert(&range->node, root);
}

/* remove range from interval tree */
static void __remove_range(struct vrange *range,
				struct rb_root *root)
{
	interval_tree_remove(&range->node, root);
}

static struct vrange *alloc_vrange(void)
{
	struct vrange *vrange = kmem_cache_alloc(vrange_cachep, GFP_KERNEL);
	if (vrange)
		atomic_set(&vrange->refcount, 1);
	return vrange;
}

static void free_vrange(struct vrange *range)
{
	lru_remove_vrange(range);
	kmem_cache_free(vrange_cachep, range);
}

static void put_vrange(struct vrange *range)
{
	WARN_ON(atomic_read(&range->refcount) < 0);
	if (atomic_dec_and_test(&range->refcount))
		free_vrange(range);
}

static inline void range_resize(struct rb_root *root,
		struct vrange *range,
		unsigned long start, unsigned long end,
		struct mm_struct *mm)
{
	__remove_range(range, root);
	__set_vrange(range, start, end);
	__add_range(range, root, mm);
}

int add_vrange(struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
	struct rb_root *root;
	struct vrange *new_range, *range;
	struct interval_tree_node *node, *next;
	int purged = 0;

	new_range = alloc_vrange();
	if (!new_range)
		return -ENOMEM;

	root = &mm->v_rb;
	vrange_lock(mm);
	node = interval_tree_iter_first(root, start, end);
	while (node) {
		next = interval_tree_iter_next(node, start, end);

		range = container_of(node, struct vrange, node);
		if (node->start < start && node->last > end) {
			put_vrange(new_range);
			goto out;
		}

		start = min_t(unsigned long, start, node->start);
		end = max_t(unsigned long, end, node->last);

		purged |= range->purged;
		__remove_range(range, root);
		put_vrange(range);

		node = next;
	}

	__set_vrange(new_range, start, end);
	new_range->purged = purged;
	__add_range(new_range, root, mm);
out:
	vrange_unlock(mm);
	return 0;
}

int remove_vrange(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	struct rb_root *root;
	struct vrange *new_range, *range;
	struct interval_tree_node *node, *next;
	int ret	= 0;
	bool used_new = false;

	new_range = alloc_vrange();
	if (!new_range)
		return -ENOMEM;

	root = &mm->v_rb;
	vrange_lock(mm);

	node = interval_tree_iter_first(root, start, end);
	while (node) {
		next = interval_tree_iter_next(node, start, end);

		range = container_of(node, struct vrange, node);
		ret |= range->purged;

		if (start <= node->start && end >= node->last) {
			__remove_range(range, root);
			put_vrange(range);
		} else if (node->start >= start) {
			range_resize(root, range, end, node->last, mm);
		} else if (node->last <= end) {
			range_resize(root, range, node->start, start, mm);
		} else {
			used_new = true;
			__set_vrange(new_range, end, node->last);
			new_range->purged = range->purged;
			new_range->mm = mm;
			range_resize(root, range, node->start, start, mm);
			__add_range(new_range, root, mm);
			break;
		}

		node = next;
	}

	vrange_unlock(mm);
	if (!used_new)
		put_vrange(new_range);

	return ret;
}

void exit_vrange(struct mm_struct *mm)
{
	struct vrange *range;
	struct rb_node *next;

	next = rb_first(&mm->v_rb);
	while (next) {
		range = vrange_entry(next);
		next = rb_next(next);
		__remove_range(range, &mm->v_rb);
		put_vrange(range);
	}
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
 *  VRANGE_NOVOLATILE - hint to kernel so VM doesn't discard vrange pages
 *		any more.
 * behavior values:
 *
 * VRANGE_FULL_MODE - Once VM start to discard pages, it discards all pages
 * 		in a vrange.
 * VRANGE_PARTIAL_MODE - VM discards some pages of all vranges by round-robin
 *
 * return values:
 *  0 - success and NOT purged.
 *  1 - at least, one of pages [start, start + len) is discarded by VM.
 *  -EINVAL - start  len < 0, start is not page-aligned, start is greater
 *		than TASK_SIZE or "mode" is not a valid value.
 *  -ENOMEM -  Short of free memory in system for successful system call.
 */
SYSCALL_DEFINE4(vrange, unsigned long, start,
		size_t, len, int, mode, int, behavior)
{
	unsigned long end;
	struct mm_struct *mm = current->mm;
	int ret = -EINVAL;

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

	if (mode == VRANGE_VOLATILE)
		ret = add_vrange(mm, start, end - 1);
	else if (mode == VRANGE_NOVOLATILE)
		ret = remove_vrange(mm, start, end - 1);
out:
	return ret;
}

bool __vrange_address(struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
	struct rb_root *root = &mm->v_rb;
	struct interval_tree_node *node;

	node = interval_tree_iter_first(root, start, end);
	return node ? true : false;
}

bool vrange_address(struct mm_struct *mm,
			unsigned long start, unsigned long end)
{
	bool ret;

	vrange_lock(mm);
	ret = __vrange_address(mm, start, end);
	vrange_unlock(mm);
	return ret;
}

static pte_t *__vpage_check_address(struct page *page,
		struct mm_struct *mm, unsigned long address, spinlock_t **ptlp)
{
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;
	bool present;

	/* TODO : look into tlbfs */
	if (unlikely(PageHuge(page)))
		return NULL;

	pmd = mm_find_pmd(mm, address);
	if (!pmd)
		return NULL;
	/*
	 * TODO : Support THP
	 */
	if (pmd_trans_huge(*pmd))
		return NULL;

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (pte_none(*pte))
		goto out;

	present = pte_present(*pte);
	if (present && page_to_pfn(page) != pte_pfn(*pte))
		goto out;
	else if (present) {
		*ptlp = ptl;
		return pte;
	} else {
		swp_entry_t entry = { .val = page_private(page) };

		VM_BUG_ON(non_swap_entry(entry));
		if (entry.val != pte_to_swp_entry(*pte).val)
			goto out;
		*ptlp = ptl;
		return pte;
	}
out:
	pte_unmap_unlock(pte, ptl);
	return NULL;
}

/*
 * This functions checks @page is matched with pte's encoded one
 * which could be a page or swap slot.
 */
static inline pte_t *vpage_check_address(struct page *page,
		struct mm_struct *mm, unsigned long address,
		spinlock_t **ptlp)
{
	pte_t *ptep;
	__cond_lock(*ptlp, ptep = __vpage_check_address(page,
				mm, address, ptlp));
	return ptep;
}

static void __vrange_purge(struct mm_struct *mm,
		unsigned long start, unsigned long end)
{
	struct rb_root *root = &mm->v_rb;
	struct vrange *range;
	struct interval_tree_node *node;

	node = interval_tree_iter_first(root, start, end);
	while (node) {
		range = container_of(node, struct vrange, node);
		range->purged = true;
		node = interval_tree_iter_next(node, start, end);
	}
}

int try_to_discard_one(struct page *page, struct vm_area_struct *vma,
		unsigned long address)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;
	int ret = 0;
	bool present;

	VM_BUG_ON(!PageLocked(page));

	vrange_lock(mm);
	pte = vpage_check_address(page, mm, address, &ptl);
	if (!pte) {
		vrange_unlock(mm);
		goto out;
	}

	if (vma->vm_flags & VM_LOCKED) {
		pte_unmap_unlock(pte, ptl);
		vrange_unlock(mm);
		return 0;
	}

	present = pte_present(*pte);
	flush_cache_page(vma, address, page_to_pfn(page));

	ptep_clear_flush(vma, address, pte);
	pteval = pte_mkvrange(*pte);

	update_hiwater_rss(mm);
	dec_mm_counter(mm, MM_ANONPAGES);

	page_remove_rmap(page);
	page_cache_release(page);
	if (!present) {
		swp_entry_t entry = pte_to_swp_entry(*pte);
		dec_mm_counter(mm, MM_SWAPENTS);
		if (unlikely(!__free_swap_and_cache(entry)))
			BUG_ON(1);
	}

	set_pte_at(mm, address, pte, pteval);
	__vrange_purge(mm, address, address + PAGE_SIZE -1);
	pte_unmap_unlock(pte, ptl);
	mmu_notifier_invalidate_page(mm, address);
	vrange_unlock(mm);
	ret = 1;
out:
	return ret;
}

static int try_to_discard_vpage(struct page *page)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	pgoff_t pgoff;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long address;
	bool ret = 0;

	anon_vma = page_lock_anon_vma_read(page);
	if (!anon_vma)
		return ret;

	pgoff = page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		pte_t *pte;
		spinlock_t *ptl;

		vma = avc->vma;
		mm = vma->vm_mm;
		address = vma_address(page, vma);

		vrange_lock(mm);
		/*
		 * We can't use page_check_address because it doesn't check
		 * swap entry of the page table. We need the check because
		 * we have to make sure atomicity of shared vrange.
		 * It means all vranges which are shared a page should be
		 * purged if a page in a process is purged.
		 */
		pte = vpage_check_address(page, mm, address, &ptl);
		if (!pte) {
			vrange_unlock(mm);
			continue;
		}

		if (vma->vm_flags & VM_LOCKED) {
			pte_unmap_unlock(pte, ptl);
			vrange_unlock(mm);
			goto out;
		}

		pte_unmap_unlock(pte, ptl);
		if (!__vrange_address(mm, address,
					address + PAGE_SIZE - 1)) {
			vrange_unlock(mm);
			goto out;
		}

		vrange_unlock(mm);
	}

	anon_vma_interval_tree_foreach(avc, &anon_vma->rb_root, pgoff, pgoff) {
		vma = avc->vma;
		address = vma_address(page, vma);
		if (!try_to_discard_one(page, vma, address))
			goto out;
	}

	ret = 1;
out:
	page_unlock_anon_vma_read(anon_vma);
	return ret;
}

int discard_vpage(struct page *page)
{
	VM_BUG_ON(!PageLocked(page));
	VM_BUG_ON(PageLRU(page));

	if (try_to_discard_vpage(page)) {
		if (PageSwapCache(page))
			try_to_free_swap(page);

		if (page_freeze_refs(page, 1)) {
			unlock_page(page);
			dec_zone_page_state(page, NR_ISOLATED_ANON);
			return 1;
		}
	}

	return 0;
}

bool is_purged_vrange(struct mm_struct *mm, unsigned long address)
{
	struct rb_root *root = &mm->v_rb;
	struct interval_tree_node *node;
	struct vrange *range;
	bool ret = false;

	vrange_lock(mm);
	node = interval_tree_iter_first(root, address, address + PAGE_SIZE - 1);
	if (node) {
		range = container_of(node, struct vrange, node);
		if (range->purged)
			ret = true;
	}
	vrange_unlock(mm);
	return ret;
}

static void vrange_pte_entry(pte_t pteval, unsigned long address,
			unsigned ptent_size, struct mm_walk *walk)
{
	struct page *page;
	struct vrange_walker_private *vwp = walk->private;
	struct vm_area_struct *vma = vwp->vma;
	struct list_head *pagelist = vwp->pagelist;
	struct zone *zone = vwp->zone;

	if (pte_none(pteval))
		return;

	if (!pte_present(pteval))
		return;

	page = vm_normal_page(vma, address, pteval);
	if (unlikely(!page))
		return;

	if (!PageLRU(page) || PageLocked(page) || !PageAnon(page))
		return;

	/* TODO : Support THP and HugeTLB */
	if (unlikely(PageCompound(page)))
		return;

	if (zone_idx(page_zone(page)) > zone_idx(zone))
		return;

	if (isolate_lru_page(page))
		return;

	list_add(&page->lru, pagelist);
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

unsigned int discard_vma_pages(struct zone *zone, struct mm_struct *mm,
		struct vm_area_struct *vma, unsigned long start,
		unsigned long end, unsigned int nr_to_discard)
{
	LIST_HEAD(pagelist);
	int ret = 0;
	struct vrange_walker_private vwp;
	struct mm_walk vrange_walk = {
		.pmd_entry = vrange_pte_range,
		.mm = vma->vm_mm,
		.private = &vwp,
	};

	vwp.pagelist = &pagelist;
	vwp.vma = vma;
	vwp.zone = zone;

	walk_page_range(start, end, &vrange_walk);

	if (!list_empty(&pagelist))
		ret = discard_vrange_page_list(zone, &pagelist);

	putback_lru_pages(&pagelist);
	return ret;
}

unsigned int discard_vrange(struct zone *zone, struct vrange *vrange,
				int nr_to_discard)
{
	struct mm_struct *mm = vrange->mm;
	unsigned long start = vrange->node.start;
	unsigned long end = vrange->node.last;
	struct vm_area_struct *vma;
	unsigned int nr_discarded = 0;

	if (!down_read_trylock(&mm->mmap_sem))
		goto out;

	vma = find_vma(mm, start);
	if (!vma || (vma->vm_start > end))
		goto out_unlock;

	for (; vma; vma = vma->vm_next) {
		if (vma->vm_start > end)
			break;

		if (vma->vm_file ||
			(vma->vm_flags & (VM_SPECIAL | VM_LOCKED)))
			continue;

		cond_resched();
		nr_discarded +=
			discard_vma_pages(zone, mm, vma,
				max_t(unsigned long, start, vma->vm_start),
				min_t(unsigned long, end + 1, vma->vm_end),
				nr_to_discard);
	}
out_unlock:
	up_read(&mm->mmap_sem);
out:
	return nr_discarded;
}

/*
 * Get next victim vrange from LRU and hold a vrange refcount
 * and vrange->mm's refcount.
 */
struct vrange *get_victim_vrange(void)
{
	struct mm_struct *mm;
	struct vrange *vrange = NULL;
	struct list_head *cur, *tmp;

	spin_lock(&lru_lock);
	list_for_each_prev_safe(cur, tmp, &lru_vrange) {
		vrange = list_entry(cur, struct vrange, lru);
		mm = vrange->mm;
		/* the process is exiting so pass it */
		if (atomic_read(&mm->mm_users) == 0) {
			list_del_init(&vrange->lru);
			vrange = NULL;
			continue;
		}

		/* vrange is freeing so continue to loop */
		if (!atomic_inc_not_zero(&vrange->refcount)) {
			list_del_init(&vrange->lru);
			vrange = NULL;
			continue;
		}

		/*
		 * we need to access mmap_sem further routine so
		 * need to get a refcount of mm.
		 * NOTE: We guarantee mm_count isn't zero in here because
		 * if we found vrange from LRU list, it means we are
		 * before exit_vrange or remove_vrange.
		 */
		atomic_inc(&mm->mm_count);

		/* Isolate vrange */
		list_del_init(&vrange->lru);
		break;
	}

	spin_unlock(&lru_lock);
	return vrange;
}

void put_victim_range(struct vrange *vrange)
{
	put_vrange(vrange);
	mmdrop(vrange->mm);
}

unsigned int discard_vrange_pages(struct zone *zone, int nr_to_discard)
{
	struct vrange *vrange, *start_vrange;
	unsigned int nr_discarded = 0;

	start_vrange = vrange = get_victim_vrange();
	if (start_vrange) {
		struct mm_struct *mm = start_vrange->mm;
		atomic_inc(&start_vrange->refcount);
		atomic_inc(&mm->mm_count);
	}

	while (vrange) {
		nr_discarded += discard_vrange(zone, vrange, nr_to_discard);
		lru_add_vrange(vrange);
		put_victim_range(vrange);

		if (nr_discarded >= nr_to_discard)
			break;

		vrange = get_victim_vrange();
		/* break if we go round the loop */
		if (vrange == start_vrange) {
			lru_add_vrange(vrange);
			put_victim_range(vrange);
			break;
		}
	}

	if (start_vrange)
		put_victim_range(start_vrange);

	return nr_discarded;
}
