#ifndef _LINUX_VRANGE_H
#define _LINUX_VRANGE_H

#include <linux/mutex.h>
#include <linux/interval_tree.h>
#include <linux/mm.h>

/* To protect race with forker */
static DECLARE_RWSEM(vrange_fork_lock);

struct vrange {
	struct interval_tree_node node;
	bool purged;
	struct mm_struct *mm;
};

#define vrange_entry(ptr) \
	container_of(ptr, struct vrange, node.rb)

#ifdef CONFIG_MMU
struct mm_struct;

static inline void mm_init_vrange(struct mm_struct *mm)
{
	mm->v_rb = RB_ROOT;
	mutex_init(&mm->v_lock);
}

static inline void vrange_lock(struct mm_struct *mm)
{
	mutex_lock(&mm->v_lock);
}

static inline void vrange_unlock(struct mm_struct *mm)
{
	mutex_unlock(&mm->v_lock);
}

extern void exit_vrange(struct mm_struct *mm);
void vrange_init(void);
int discard_vpage(struct page *page);
bool vrange_address(struct mm_struct *mm, unsigned long start,
			unsigned long end);

extern bool is_purged_vrange(struct mm_struct *mm, unsigned long address);

#else

static inline void vrange_init(void) {};
static inline void mm_init_vrange(struct mm_struct *mm) {};
static inline void exit_vrange(struct mm_struct *mm);

static inline bool vrange_address(struct mm_struct *mm, unsigned long start,
		unsigned long end) { return false; };
static inline int discard_vpage(struct page *page) { return 0 };
#endif
#endif /* _LINIUX_VRANGE_H */
