/*
 *  include/linux/anon_inodes.h
 *
 *  Copyright (C) 2016  Minchan Kim <minchan@kernel.org>
 *
 */

#ifndef _LINUX_MOVABLE_INODES_H
#define _LINUX_MOVABLE_INODES_H

struct file_operations;
struct address_space_operations;

struct file *movable_getfile(const char *name,
			const struct file_operations *fops,
			const struct address_space_operations *aop,
			void *priv, int flags);

#endif /* _LINUX_MOVABLE_INODES_H */

