#ifndef _LINUX_VRANGE_TYPES_H
#define _LINUX_VRANGE_TYPES_H

#include <linux/mutex.h>
#include <linux/interval_tree.h>

enum vrange_type {
	VRANGE_MM,
	VRANGE_FILE,
};

struct vrange_root {
	struct rb_root v_rb;		/* vrange rb tree */
	struct mutex v_lock;		/* Protect v_rb */
	enum vrange_type type;		/* range root type */
	atomic_t refcount;
};

struct vrange {
	struct interval_tree_node node;
	struct vrange_root *owner;
	int purged;
	struct list_head lru;
	atomic_t refcount;
};
#endif

