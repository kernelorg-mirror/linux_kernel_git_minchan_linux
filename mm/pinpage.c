#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pinpage.h>
#include <linux/pagemap.h>

static DEFINE_SPINLOCK(pinpage_system_lock);
static LIST_HEAD(pinpage_system_list);

struct pinpage_info {
	unsigned long pfn;
	void *private;
};

int general_create_subsys(struct pinpage_system *psys)
{
	INIT_RADIX_TREE(&psys->page_tree, GFP_KERNEL);
	spin_lock_init(&psys->tree_lock);
	return 0;
}
EXPORT_SYMBOL(general_create_subsys);

int general_destroy_subsys(struct pinpage_system *psys)
{
	return 0;
}
EXPORT_SYMBOL(general_destroy_subsys);

int general_add_page(struct pinpage_system *psys, struct page *page,
			void *private)
{
	int ret = -ENOMEM;
	unsigned long pfn = page_to_pfn(page);
	struct pinpage_info *pinfo = kmalloc(sizeof(pinfo), GFP_KERNEL);
	if (!pinfo)
		return ret;

	pinfo->pfn = pfn;
	pinfo->private = private;

	spin_lock(&psys->tree_lock);
	ret = radix_tree_insert(&psys->page_tree, pfn, pinfo);
	spin_unlock(&psys->tree_lock);
	return ret;
}
EXPORT_SYMBOL(general_add_page);

int general_del_page(struct pinpage_system *psys, struct page *page)
{
	struct pinpage_info *pinfo;
	spin_lock(&psys->tree_lock);
	pinfo = radix_tree_lookup(&psys->page_tree, page_to_pfn(page));
	if (!pinfo) {
		spin_unlock(&psys->tree_lock);
		return -EINVAL;
	}
	radix_tree_delete(&psys->page_tree, page_to_pfn(page));
	spin_unlock(&psys->tree_lock);
	return 0;
}
EXPORT_SYMBOL(general_del_page);

int general_find_page(struct pinpage_system *psys, struct page *page)
{
	struct pinpage_info *pinfo;
	spin_lock(&psys->tree_lock);
	pinfo = radix_tree_lookup(&psys->page_tree, page_to_pfn(page));
	spin_unlock(&psys->tree_lock);
	return pinfo ? 1 : 0;
}
EXPORT_SYMBOL(general_find_page);

int set_pinpage(struct pinpage_system *psys, struct page *page, void *private)
{
	int ret;
	ret = psys->add_page(psys, page, private);
	if (!ret) {
		lock_page(page);
		/* Doesn't allow nesting */
		VM_BUG_ON(PagePin(page));
		SetPagePin(page);
		unlock_page(page);
	}
	return ret;
}
EXPORT_SYMBOL(set_pinpage);

int clear_pinpage(struct pinpage_system *psys, struct page *page)
{
	int ret;
	ret = psys->del_page(psys, page);
	if (!ret) {
		lock_page(page);
		ClearPagePin(page);
		unlock_page(page);
	}
	return ret;
}
EXPORT_SYMBOL(clear_pinpage);

int register_pinpage(struct pinpage_system *psys)
{
	/* register pinpage_subsystem to global list */	
	spin_lock(&pinpage_system_lock);
	list_add(&psys->list, &pinpage_system_list);
	spin_unlock(&pinpage_system_lock);
	return psys->create_subsys(psys);
}
EXPORT_SYMBOL(register_pinpage);

int unregister_pinpage(struct pinpage_system *psys)
{
	/* register pinpage_subsystem to global list */	
	spin_lock(&pinpage_system_lock);
	list_del(&psys->list);
	spin_unlock(&pinpage_system_lock);
	return psys->destroy_subsys(psys);
}
EXPORT_SYMBOL(unregister_pinpage);

int migrate_pinpage(struct page *page, struct page *newpage)
{
	int err = 0;
	struct pinpage_system *psys;

	spin_lock(&pinpage_system_lock);
	list_for_each_entry(psys, &pinpage_system_list, list) {
		if (psys->find_page(psys, page)) {
			err = psys->migrate(psys, page, newpage);
			break;
		}
	}
	spin_unlock(&pinpage_system_lock);
	return err;
}
