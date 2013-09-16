/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009
 * Phillip Lougher <phillip@squashfs.org.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * decompressor.c
 */

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "decompressor.h"
#include "squashfs.h"

/*
 * This file (and decompressor.h) implements a decompressor framework for
 * Squashfs, allowing multiple decompressors to be easily supported
 */

static const struct squashfs_decompressor squashfs_lzma_unsupported_comp_ops = {
	NULL, NULL, NULL, LZMA_COMPRESSION, "lzma", 0
};

#ifndef CONFIG_SQUASHFS_LZO
static const struct squashfs_decompressor squashfs_lzo_comp_ops = {
	NULL, NULL, NULL, LZO_COMPRESSION, "lzo", 0
};
#endif

#ifndef CONFIG_SQUASHFS_XZ
static const struct squashfs_decompressor squashfs_xz_comp_ops = {
	NULL, NULL, NULL, XZ_COMPRESSION, "xz", 0
};
#endif

#ifndef CONFIG_SQUASHFS_ZLIB
static const struct squashfs_decompressor squashfs_zlib_comp_ops = {
	NULL, NULL, NULL, ZLIB_COMPRESSION, "zlib", 0
};
#endif

static const struct squashfs_decompressor squashfs_unknown_comp_ops = {
	NULL, NULL, NULL, 0, "unknown", 0
};

static const struct squashfs_decompressor *decompressor[] = {
	&squashfs_zlib_comp_ops,
	&squashfs_lzo_comp_ops,
	&squashfs_xz_comp_ops,
	&squashfs_lzma_unsupported_comp_ops,
	&squashfs_unknown_comp_ops
};

void squashfs_decompressor_free(struct squashfs_sb_info *msblk,
	struct squashfs_decomp_strm *stream)
{
	if (msblk->decompressor)
		msblk->decompressor->free(stream->strm);
	kfree(stream);
}

static void *squashfs_get_decomp_strm(struct squashfs_sb_info *msblk)
{
	struct squashfs_decomp_strm *strm = NULL;
	mutex_lock(&msblk->comp_strm_mutex);
	if (!list_empty(&msblk->strm_list)) {
		strm = list_entry(msblk->strm_list.next,
				struct squashfs_decomp_strm, list);
		list_del(&strm->list);
		msblk->nr_avail_decomp--;
		WARN_ON(msblk->nr_avail_decomp < 0);
	}
	mutex_unlock(&msblk->comp_strm_mutex);
	return strm;
}

static bool full_decomp_strm(struct squashfs_sb_info *msblk)
{
	/* MM do readahread 2M unit */
	int blocks = 2 * 1024 * 1024 / msblk->block_size;
	return msblk->nr_avail_decomp > (num_online_cpus() * blocks * 2);
}

static void squashfs_put_decomp_strm(struct squashfs_sb_info *msblk,
					struct squashfs_decomp_strm *strm)
{
	mutex_lock(&msblk->comp_strm_mutex);
	if (full_decomp_strm(msblk)) {
		mutex_unlock(&msblk->comp_strm_mutex);
		squashfs_decompressor_free(msblk, strm);
		return;
	}

	list_add(&strm->list, &msblk->strm_list);
	msblk->nr_avail_decomp++;
	mutex_unlock(&msblk->comp_strm_mutex);
	wake_up(&msblk->decomp_wait_queue);
}

int squashfs_decompress(struct super_block *sb, void **buffer,
			struct buffer_head **bh, int b, int offset, int length,
			int srclength, int pages)
{
	int ret;
	struct squashfs_decomp_strm *strm;
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	while (1) {
		strm = squashfs_get_decomp_strm(msblk);
		if (strm)
			break;

		if (!full_decomp_strm(msblk)) {
			strm = squashfs_decompressor_init(sb);
			if (strm)
				break;
		}

		wait_event(msblk->decomp_wait_queue, msblk->nr_avail_decomp);
		continue;
	}

	ret = msblk->decompressor->decompress(msblk, strm->strm, buffer, bh,
		b, offset, length, srclength, pages);

	squashfs_put_decomp_strm(msblk, strm);
	return ret;
}

const struct squashfs_decompressor *squashfs_lookup_decompressor(int id)
{
	int i;

	for (i = 0; decompressor[i]->id; i++)
		if (id == decompressor[i]->id)
			break;

	return decompressor[i];
}

struct squashfs_decomp_strm *squashfs_decompressor_init(struct super_block *sb)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct squashfs_decomp_strm *decomp_strm = NULL;
	void *strm, *buffer = NULL;
	int length = 0;

	decomp_strm = kmalloc(sizeof(struct squashfs_decomp_strm), GFP_KERNEL);
	if (!decomp_strm)
		return ERR_PTR(-ENOMEM);
	/*
	 * Read decompressor specific options from file system if present
	 */
	if (SQUASHFS_COMP_OPTS(msblk->flags)) {
		buffer = kmalloc(PAGE_CACHE_SIZE, GFP_KERNEL);
		if (buffer == NULL) {
			decomp_strm = ERR_PTR(-ENOMEM);
			goto finished;
		}

		length = squashfs_read_metablock(sb, &buffer,
			sizeof(struct squashfs_super_block), 0, NULL,
			PAGE_CACHE_SIZE, 1);

		if (length < 0) {
			decomp_strm = ERR_PTR(length);
			goto finished;
		}
	}

	strm = msblk->decompressor->init(msblk, buffer, length);
	if (IS_ERR(strm)) {
		decomp_strm = strm;
		goto finished;
	}

	decomp_strm->strm = strm;
	kfree(buffer);
	return decomp_strm;

finished:
	kfree(decomp_strm);
	kfree(buffer);
	return decomp_strm;
}
