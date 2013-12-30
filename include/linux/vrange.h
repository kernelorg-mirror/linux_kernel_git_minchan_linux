#ifndef _LINUX_VRANGE_H
#define _LINUX_VRANGE_H

#include <linux/vrange_types.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#define vrange_from_node(node_ptr) \
	container_of(node_ptr, struct vrange, node)

#define vrange_entry(ptr) \
	container_of(ptr, struct vrange, node.rb)

struct scan_control;

#ifdef CONFIG_MMU

static inline swp_entry_t make_vrange_entry(void)
{
	return swp_entry(SWP_VRANGE, 0);
}

static inline int is_vrange_entry(swp_entry_t entry)
{
	return swp_type(entry) == SWP_VRANGE;
}

static inline void vrange_root_init(struct vrange_root *vroot, int type,
								void *object)
{
	vroot->type = type;
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

extern int vrange_clear(struct vrange_root *vroot,
				unsigned long start, unsigned long end);
extern void vrange_root_cleanup(struct vrange_root *vroot);
extern int vrange_fork(struct mm_struct *new,
					struct mm_struct *old);
int discard_vpage(struct page *page);
bool vrange_addr_volatile(struct vm_area_struct *vma, unsigned long addr);
extern bool vrange_addr_purged(struct vm_area_struct *vma,
				unsigned long address);
extern unsigned long shrink_vrange(enum lru_list lru, struct lruvec *lruvec,
					struct scan_control *sc);
#else

static inline void vrange_root_init(struct vrange_root *vroot,
					int type, void *object) {};
static inline void vrange_root_cleanup(struct vrange_root *vroot) {};
static inline int vrange_fork(struct mm_struct *new, struct mm_struct *old)
{
	return 0;
}

static inline bool vrange_addr_volatile(struct vm_area_struct *vma,
					unsigned long addr)
{
	return false;
}
static inline int discard_vpage(struct page *page) { return 0 };
static inline bool vrange_addr_purged(struct vm_area_struct *vma,
				unsigned long address);
static inline unsigned long shrink_vrange(enum lru_list lru,
		struct lruvec *lruvec, 	struct scan_control *sc)
{
	return 0;
}

#endif
#endif /* _LINIUX_VRANGE_H */
