#ifndef _LINUX_PINPAGE_H
#define _LINUX_PINPAGE_H

#include <linux/radix-tree.h>

/*
 * NOTE : pinpage_system user shouldn't use page->lru and page->flags
 * fields.
 */
struct pinpage_system {
	struct radix_tree_root page_tree;
	spinlock_t tree_lock;

	int (*create_subsys)(struct pinpage_system *psys);
	int (*destroy_subsys)(struct pinpage_system *psys);
	int (*migrate)(struct pinpage_system *psys, struct page *page,
			struct page *newpage);
	int (*add_page)(struct pinpage_system *psys, struct page *page,
			void *private);
	int (*del_page)(struct pinpage_system *psys, struct page *page);
	int (*find_page)(struct pinpage_system *psys, struct page *page);

	struct list_head list;
};

extern int general_create_subsys(struct pinpage_system *psys);
extern int general_destroy_subsys(struct pinpage_system *psys);
extern int general_add_page(struct pinpage_system *psys, struct page *page,
			void *private);
extern int general_del_page(struct pinpage_system *psys, struct page *page);
extern int general_find_page(struct pinpage_system *psys, struct page *page);

extern int set_pinpage(struct pinpage_system *psys, struct page *page,
		void *private);
extern int register_pinpage(struct pinpage_system *psys);
extern int migrate_pinpage(struct page *page, struct page *newpage);

#endif

