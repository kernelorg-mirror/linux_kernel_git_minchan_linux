/*
 * mm/vrange.c
 */

#include <linux/vrange.h>
#include <linux/slab.h>

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
