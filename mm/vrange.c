/*
 * mm/vrange.c
 */

#include <linux/vrange.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/mman.h>

static struct kmem_cache *vrange_cachep;

static int __init vrange_init(void)
{
	vrange_cachep = KMEM_CACHE(vrange, SLAB_PANIC);
	return 0;
}
module_init(vrange_init);

static struct vrange *__vrange_alloc(gfp_t flags)
{
	struct vrange *vrange = kmem_cache_alloc(vrange_cachep, flags);
	if (!vrange)
		return vrange;
	vrange->owner = NULL;
	vrange->purged = 0;
	return vrange;
}

static void __vrange_free(struct vrange *range)
{
	WARN_ON(range->owner);
	kmem_cache_free(vrange_cachep, range);
}

static void __vrange_add(struct vrange *range, struct vrange_root *vroot)
{
	range->owner = vroot;
	interval_tree_insert(&range->node, &vroot->v_rb);
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
	__vrange_set(range, start_idx, end_idx, purged);
	__vrange_add(range, vroot);
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
			__vrange_free(new_range);
			goto out;
		}

		start_idx = min_t(unsigned long, start_idx, node->start);
		end_idx = max_t(unsigned long, end_idx, node->last);
		purged |= range->purged;

		__vrange_remove(range);
		__vrange_free(range);

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
			__vrange_free(range);
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
					range->purged);
			__vrange_add(new_range, vroot);
			break;
		}

		node = next;
	}
	vrange_unlock(vroot);

	if (!used_new)
		__vrange_free(new_range);

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

	vrange_lock(vroot);
	/* We should remove node by post-order traversal */
	while ((node = rb_first(&vroot->v_rb))) {
		range = vrange_entry(node);
		__vrange_remove(range);
		__vrange_free(range);
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
	vrange_unlock(old);
	vrange_root_cleanup(new);
	return -ENOMEM;
}

static inline struct vrange_root *__vma_to_vroot(struct vm_area_struct *vma)
{
	struct vrange_root *vroot = NULL;

	if (vma->vm_file && (vma->vm_flags & VM_SHARED))
		vroot = &vma->vm_file->f_mapping->vroot;
	else
		vroot = &vma->vm_mm->vroot;
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
