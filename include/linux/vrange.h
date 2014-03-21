#ifndef _LINUX_VRANGE_H
#define _LINUX_VRANGE_H

#include <linux/swap.h>
#include <linux/swapops.h>

#define VRANGE_NONVOLATILE 0
#define VRANGE_VOLATILE 1
#define VRANGE_VALID_FLAGS (0) /* Don't yet support any flags */

extern int discard_vpage(struct page *page);

#endif /* _LINUX_VRANGE_H */
