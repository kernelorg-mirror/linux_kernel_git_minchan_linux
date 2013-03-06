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

static struct kmem_cache *vrange_cachep;

void __init vrange_init(void)
{
	vrange_cachep = KMEM_CACHE(vrange, SLAB_PANIC);
}

static inline void __set_vrange(struct vrange *range,
		unsigned long start_idx, unsigned long end_idx)
{
	range->node.start = start_idx;
	range->node.last = end_idx;
}

static void __add_range(struct vrange *range,
			struct rb_root *root, struct mm_struct *mm)
{
	range->mm = mm;
	interval_tree_insert(&range->node, root);
}

static void __remove_range(struct vrange *range,
				struct rb_root *root)
{
	interval_tree_remove(&range->node, root);
}

static struct vrange *alloc_vrange(void)
{
	return kmem_cache_alloc(vrange_cachep, GFP_KERNEL);
}

static void free_vrange(struct vrange *range)
{
	kmem_cache_free(vrange_cachep, range);
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
			free_vrange(new_range);
			goto out;
		}

		start = min_t(unsigned long, start, node->start);
		end = max_t(unsigned long, end, node->last);

		purged |= range->purged;
		__remove_range(range, root);
		free_vrange(range);

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
			free_vrange(range);
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
		free_vrange(new_range);

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
		free_vrange(range);
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
