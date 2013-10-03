/*
 * mm/vrange.c
 */

#include <linux/vrange.h>
#include <linux/slab.h>

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
