#include <linux/syscalls.h>
#include <linux/vrange.h>
#include <linux/mm_inline.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/mmu_notifier.h>
#include <linux/mm_inline.h>
#include "internal.h"

struct vrange_walker {
	struct vm_area_struct *vma;
	int page_was_purged;
};


/**
 * vrange_check_purged_pte - Checks ptes for purged pages
 *
 * Iterates over the ptes in the pmd checking if they have
 * purged swap entries.
 *
 * Sets the vrange_walker.pages_purged to 1 if any were purged.
 */
static int vrange_check_purged_pte(pmd_t *pmd, unsigned long addr,
					unsigned long end, struct mm_walk *walk)
{
	struct vrange_walker *vw = walk->private;
	pte_t *pte;
	spinlock_t *ptl;

	if (pmd_trans_huge(*pmd))
		return 0;
	if (pmd_trans_unstable(pmd))
		return 0;

	pte = pte_offset_map_lock(walk->mm, pmd, addr, &ptl);
	for (; addr != end; pte++, addr += PAGE_SIZE) {
		if (!pte_present(*pte)) {
			swp_entry_t vrange_entry = pte_to_swp_entry(*pte);

			if (unlikely(is_vpurged_entry(vrange_entry))) {
				vw->page_was_purged = 1;
				break;
			}
		}
	}
	pte_unmap_unlock(pte - 1, ptl);
	cond_resched();

	return 0;
}


/**
 * vrange_check_purged - Sets up a mm_walk to check for purged pages
 *
 * Sets up and calls wa_page_range() to check for purge pages.
 *
 * Returns 1 if pages in the range were purged, 0 otherwise.
 */
static int vrange_check_purged(struct mm_struct *mm,
					 struct vm_area_struct *vma,
					 unsigned long start,
					 unsigned long end)
{
	struct vrange_walker vw;
	struct mm_walk vrange_walk = {
		.pmd_entry = vrange_check_purged_pte,
		.mm = vma->vm_mm,
		.private = &vw,
	};
	vw.page_was_purged = 0;
	vw.vma = vma;

	walk_page_range(start, end, &vrange_walk);

	return vw.page_was_purged;

}

/**
 * do_vrange - Marks or clears VMAs in the range (start-end) as VM_VOLATILE
 *
 * Core logic of sys_volatile. Iterates over the VMAs in the specified
 * range, and marks or clears them as VM_VOLATILE, splitting or merging them
 * as needed.
 *
 * Returns the number of bytes successfully modified.
 *
 * Returns error only if no bytes were modified.
 */
static ssize_t do_vrange(struct mm_struct *mm, unsigned long start,
				unsigned long end, unsigned long mode,
				unsigned long flags, int *purged)
{
	struct vm_area_struct *vma, *prev;
	unsigned long orig_start = start;
	ssize_t count = 0, ret = 0;

	down_read(&mm->mmap_sem);

	vma = find_vma_prev(mm, start, &prev);
	if (vma && start > vma->vm_start)
		prev = vma;

	for (;;) {
		unsigned long new_flags;
		pgoff_t pgoff;
		unsigned long tmp;

		if (!vma)
			goto out;

		if (vma->vm_flags & (VM_SPECIAL|VM_LOCKED|VM_MIXEDMAP|
					VM_HUGETLB))
			goto out;

		/* We don't support volatility on files for now */
		if (vma->vm_file) {
			ret = -EINVAL;
			goto out;
		}

		/* return ENOMEM if we're trying to mark unmapped pages */
		if (start < vma->vm_start) {
			ret = -ENOMEM;
			goto out;
		}

		new_flags = vma->vm_flags;

		tmp = vma->vm_end;
		if (end < tmp)
			tmp = end;

		switch (mode) {
		case VRANGE_VOLATILE:
			new_flags |= VM_VOLATILE;
			break;
		case VRANGE_NONVOLATILE:
			new_flags &= ~VM_VOLATILE;
		}

		pgoff = vma->vm_pgoff + ((start - vma->vm_start) >> PAGE_SHIFT);
		prev = vma_merge(mm, prev, start, tmp, new_flags,
					vma->anon_vma, vma->vm_file, pgoff,
					vma_policy(vma));
		if (prev)
			goto success;

		if (start != vma->vm_start) {
			ret = split_vma(mm, vma, start, 1);
			if (ret)
				goto out;
		}

		if (tmp != vma->vm_end) {
			ret = split_vma(mm, vma, tmp, 0);
			if (ret)
				goto out;
		}

		prev = vma;
success:
		vma->vm_flags = new_flags;

		/* update count to distance covered so far*/
		count = tmp - orig_start;

		start = tmp;
		if (start < prev->vm_end)
			start = prev->vm_end;
		if (start >= end)
			goto out;
		vma = prev->vm_next;
	}
out:
	if (count && (mode == VRANGE_NONVOLATILE))
		*purged = vrange_check_purged(mm, vma,
						orig_start,
						orig_start+count);

	up_read(&mm->mmap_sem);

	/* report bytes successfully marked, even if we're exiting on error */
	if (count)
		return count;

	return ret;
}


/**
 * sys_vrange - Marks specified range as volatile or non-volatile.
 *
 * Validates the syscall inputs and calls do_vrange(), then copies the
 * purged flag back out to userspace.
 *
 * Returns the number of bytes successfully modified.
 * Returns error only if no bytes were modified.
 */
SYSCALL_DEFINE5(vrange, unsigned long, start, size_t, len, unsigned long, mode,
			unsigned long, flags, int __user *, purged)
{
	unsigned long end;
	struct mm_struct *mm = current->mm;
	ssize_t ret = -EINVAL;
	int p = 0;

	if (flags & ~VRANGE_VALID_FLAGS)
		goto out;

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

	ret = do_vrange(mm, start, end, mode, flags, &p);

	if (purged) {
		if (put_user(p, purged)) {
			/*
			 * This would be bad, since we've modified volatilty
			 * and the change in purged state would be lost.
			 */
			WARN_ONCE(1, "vrange: purge state possibly lost\n");
		}
	}

out:
	return ret;
}


/**
 * try_to_discard_one - Purge a volatile page from a vma
 *
 * Finds the pte for a page in a vma, marks the pte as purged
 * and release the page.
 */
static void try_to_discard_one(struct page *page, struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	pte_t *pte;
	pte_t pteval;
	spinlock_t *ptl;
	unsigned long addr;

	VM_BUG_ON(!PageLocked(page));

	addr = vma_address(page, vma);
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

	set_pte_at(mm, addr, pte,
				swp_entry_to_pte(make_vpurged_entry()));

	pte_unmap_unlock(pte, ptl);
	mmu_notifier_invalidate_page(mm, addr);

}

/**
 * try_to_discard_vpage - check vma chain and discard from vmas marked volatile
 *
 * Goes over all the vmas that hold a page, and where the vmas are volatile,
 * purge the page from the vma.
 *
 * Returns 0 on success, -1 on error.
 */
static int try_to_discard_vpage(struct page *page)
{
	struct anon_vma *anon_vma;
	struct anon_vma_chain *avc;
	pgoff_t pgoff;

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
		struct vm_area_struct *vma = avc->vma;

		if (!(vma->vm_flags & VM_VOLATILE))
			continue;
		try_to_discard_one(page, vma);
	}
	page_unlock_anon_vma_read(anon_vma);
	return 0;
}


/**
 * discard_vpage - If possible, discard the specified volatile page
 *
 * Attempts to discard a volatile page, and if needed frees the swap page
 *
 * Returns 0 on success, -1 on error.
 */
int discard_vpage(struct page *page)
{
	VM_BUG_ON(!PageLocked(page));
	VM_BUG_ON(PageLRU(page));

	/* XXX - for now we only support anonymous volatile pages */
	if (!PageAnon(page))
		return -1;

	if (!try_to_discard_vpage(page)) {
		if (PageSwapCache(page))
			try_to_free_swap(page);

		if (page_freeze_refs(page, 1)) {
			unlock_page(page);
			return 0;
		}
	}

	return -1;
}
