/*
 *  fs/movabile_inodes.c
 *
 *  Copyright (C) 2016  Minchan Kim <minchan@kernel.org>
 */
#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/file.h>

#define MOVABLE_MAGIC                  0x58275827

static struct vfsmount *movable_mnt;

struct file *movable_getfile(const char *name,
			const struct file_operations *fop,
			const struct address_space_operations *a_ops,
			void *priv, int flags)
{
	struct qstr this = QSTR_INIT("[movable]", 9);
	struct file *file;
	struct path path;
	struct inode *inode = new_inode_pseudo(movable_mnt->mnt_sb);

	if (IS_ERR(inode))
		return ERR_CAST(inode);

	inode->i_mapping->a_ops = a_ops;
	path.dentry = d_alloc_pseudo(movable_mnt->mnt_sb, &this);
	if (!path.dentry) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	path.mnt = mntget(movable_mnt);

	d_instantiate(path.dentry, inode);

	file = alloc_file(&path, OPEN_FMODE(flags), fop);
	if (IS_ERR(file)) {
		path_put(&path);
		return file;
	}

	file->f_flags = flags;
	file->private_data = priv;
	file->f_mapping->private_data = priv;

	return file;
}
EXPORT_SYMBOL_GPL(movable_getfile);

static struct dentry *movable_mount(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data)
{
	static const struct dentry_operations ops = {
		.d_dname	= simple_dname,
	};
	return mount_pseudo(fs_type, "movable:", NULL, &ops, MOVABLE_MAGIC);
}

static int __init movable_setup(void)
{
	static struct file_system_type movable_fs = {
		.name		= "movable",
		.mount		= movable_mount,
		.kill_sb	= kill_anon_super,
	};
	movable_mnt = kern_mount(&movable_fs);
	if (IS_ERR(movable_mnt))
		panic("Failed to create movable fs mount.");

	return 0;
}
__initcall(movable_setup);
