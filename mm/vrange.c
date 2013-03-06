/*
 * mm/vrange.c
 */

#include <linux/vrange.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/mman.h>

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
				struct rb_root *root)
{
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
		unsigned long start, unsigned long end)
{
	__remove_range(range, root);
	__set_vrange(range, start, end);
	__add_range(range, root);
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

	__add_range(new_range, root);
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
			range_resize(root, range, end, node->last);
		} else if (node->last <= end) {
			range_resize(root, range, node->start, start);
		} else {
			used_new = true;
			__set_vrange(new_range, end, node->last);
			new_range->purged = range->purged;
			range_resize(root, range, node->start, start);
			__add_range(new_range, root);
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
