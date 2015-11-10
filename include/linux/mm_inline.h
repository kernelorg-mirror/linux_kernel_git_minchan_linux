#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns true if @page is page cache page backed by a regular filesystem,
 * or false if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline bool page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int nr_pages = hpage_nr_pages(page);

	if (lru == LRU_LZFREE)
		VM_BUG_ON_PAGE(PageActive(page), page);

	mem_cgroup_update_lru_size(lruvec, lru, nr_pages);
	list_add(&page->lru, &lruvec->lists[lru]);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, nr_pages);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec, enum lru_list lru)
{
	int nr_pages = hpage_nr_pages(page);

	if (lru == LRU_LZFREE)
		VM_BUG_ON_PAGE(!PageLazyFree(page), page);

	mem_cgroup_update_lru_size(lruvec, lru, -nr_pages);
	list_del(&page->lru);
	__mod_zone_page_state(lruvec_zone(lruvec), NR_LRU_BASE + lru, -nr_pages);
}

/**
 * page_lru_base_type - which LRU list type should a page be on?
 * @page: the page to test
 *
 * Used for LRU list index arithmetic.
 *
 * Returns the base LRU type - file or anon or lazyfree - @page should be on.
 */
static inline enum lru_list page_lru_base_type(struct page *page)
{
	if (page_is_file_cache(page))
		return LRU_INACTIVE_FILE;
	if (PageLazyFree(page))
		return LRU_LZFREE;
	return LRU_INACTIVE_ANON;
}

/**
 * lru_index - which LRU list is lru on for accouting update_page_reclaim_stat
 *
 * Used for LRU list index arithmetic.
 *
 * Returns 0 if @lru is anon, 1 if it is file, 2 if it is lazyfree
 */
static inline int lru_index(enum lru_list lru)
{
	int base;

	switch (lru) {
	case LRU_INACTIVE_ANON:
	case LRU_ACTIVE_ANON:
		base = 0;
		break;
	case LRU_INACTIVE_FILE:
	case LRU_ACTIVE_FILE:
		base = 1;
		break;
	case LRU_LZFREE:
		base = 2;
		break;
	default:
		BUG();
	}
	return base;
}

/*
 * page_off_isolate - which LRU list was page on for accouting NR_ISOLATED.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of
 * zone_page_state;
 */
static inline int page_off_isolate(struct page *page)
{
	int lru = NR_ISOLATED_LZFREE;

	if (!PageSwapBacked(page))
		lru = NR_ISOLATED_FILE;
	else if (PageLazyFree(page))
		lru = NR_ISOLATED_LZFREE;
	return lru;
}

/**
 * lru_off_isolate - which LRU list was @lru on for accouting NR_ISOLATED.
 * @lru: the lru to test
 *
 * Returns the LRU list a page was on, as an index into the array of
 * zone_page_state;
 */
static inline int lru_off_isolate(enum lru_list lru)
{
	int base = NR_ISOLATED_LZFREE;

	if (lru <= LRU_ACTIVE_ANON)
		base = NR_ISOLATED_ANON;
	else if (lru <= LRU_ACTIVE_FILE)
		base = NR_ISOLATED_FILE;
	return base;
}

/**
 * page_off_lru - which LRU list was page on? clearing its lru flags.
 * @page: the page to test
 *
 * Returns the LRU list a page was on, as an index into the array of LRU
 * lists; and clears its Unevictable or Active flags, ready for freeing.
 */
static __always_inline enum lru_list page_off_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page)) {
		__ClearPageUnevictable(page);
		lru = LRU_UNEVICTABLE;
	} else {
		lru = page_lru_base_type(page);
		if (PageActive(page)) {
			__ClearPageActive(page);
			lru += LRU_ACTIVE;
		}
	}
	return lru;
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	if (PageUnevictable(page))
		lru = LRU_UNEVICTABLE;
	else {
		lru = page_lru_base_type(page);
		if (PageActive(page))
			lru += LRU_ACTIVE;
	}
	return lru;
}

#endif
