#ifndef _LINUX_VRANGE_H
#define _LINUX_VRANGE_H

#include <linux/vrange_types.h>
#include <linux/mm.h>

#define vrange_from_node(node_ptr) \
	container_of(node_ptr, struct vrange, node)

#define vrange_entry(ptr) \
	container_of(ptr, struct vrange, node.rb)

#ifdef CONFIG_MMU

static inline void vrange_root_init(struct vrange_root *vroot, int type,
								void *object)
{
	vroot->type = type;
	vroot->v_rb = RB_ROOT;
	mutex_init(&vroot->v_lock);
	vroot->object = object;
}

static inline void vrange_lock(struct vrange_root *vroot)
{
	mutex_lock(&vroot->v_lock);
}

static inline void vrange_unlock(struct vrange_root *vroot)
{
	mutex_unlock(&vroot->v_lock);
}

static inline int vrange_type(struct vrange *vrange)
{
	return vrange->owner->type;
}

extern void vrange_root_cleanup(struct vrange_root *vroot);
extern int vrange_fork(struct mm_struct *new,
					struct mm_struct *old);
#else

static inline void vrange_root_init(struct vrange_root *vroot,
					int type, void *object) {};
static inline void vrange_root_cleanup(struct vrange_root *vroot) {};
static inline int vrange_fork(struct mm_struct *new, struct mm_struct *old)
{
	return 0;
}

#endif
#endif /* _LINIUX_VRANGE_H */
