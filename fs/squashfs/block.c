/*
 * Squashfs - a compressed read only filesystem for Linux
 *
 * Copyright (c) 2002, 2003, 2004, 2005, 2006, 2007, 2008
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
 * block.c
 */

/*
 * This file implements the low-level routines to read and decompress
 * datablocks and metadata blocks.
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/buffer_head.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "squashfs.h"
#include "decompressor.h"

/*
 * Read the metadata block length, this is stored in the first two
 * bytes of the metadata block.
 */
static struct buffer_head *get_block_length(struct super_block *sb,
			u64 *cur_index, int *offset, int *length)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct buffer_head *bh;

	bh = sb_bread(sb, *cur_index);
	if (bh == NULL)
		return NULL;

	if (msblk->devblksize - *offset == 1) {
		*length = (unsigned char) bh->b_data[*offset];
		put_bh(bh);
		bh = sb_bread(sb, ++(*cur_index));
		if (bh == NULL)
			return NULL;
		*length |= (unsigned char) bh->b_data[0] << 8;
		*offset = 1;
	} else {
		*length = (unsigned char) bh->b_data[*offset] |
			(unsigned char) bh->b_data[*offset + 1] << 8;
		*offset += 2;

		if (*offset == msblk->devblksize) {
			put_bh(bh);
			bh = sb_bread(sb, ++(*cur_index));
			if (bh == NULL)
				return NULL;
			*offset = 0;
		}
	}

	return bh;
}



int squashfs_decompress_block(struct super_block *sb, int compressed,
		void **buffer, struct buffer_head **bh, int nr_bh,
		int offset, int length, int srclength, int pages)
{
	int k = 0;

	if (compressed) {
		length = squashfs_decompress(sb, buffer, bh, nr_bh,
				offset, length, srclength, pages);
		if (length < 0)
			goto out;
	} else {
		/*
		 * Block is uncompressed.
		 */
		struct squashfs_sb_info *msblk = sb->s_fs_info;
		int bytes, in, avail, pg_offset = 0, page = 0;

		for (bytes = length; k < nr_bh; k++) {
			in = min(bytes, msblk->devblksize - offset);
			bytes -= in;
			wait_on_buffer(bh[k]);
			if (!buffer_uptodate(bh[k]))
				goto block_release;
			while (in) {
				if (pg_offset == PAGE_CACHE_SIZE) {
					page++;
					pg_offset = 0;
				}
				avail = min_t(int, in, PAGE_CACHE_SIZE -
						pg_offset);
				memcpy(buffer[page] + pg_offset,
						bh[k]->b_data + offset, avail);
				in -= avail;
				pg_offset += avail;
				offset += avail;
			}
			offset = 0;
			put_bh(bh[k]);
		}
	}

	return length;

block_release:
	for (; k < nr_bh; k++)
		put_bh(bh[k]);
out:
	return length;
}

int squashfs_read_submit(struct super_block *sb, u64 index, int length,
			int srclength, struct buffer_head **bh, int *nr_bh)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	int offset = index & ((1 << msblk->devblksize_log2) - 1);
	u64 cur_index = index >> msblk->devblksize_log2;
	int bytes, b = 0, k = 0;

	bytes = -offset;
	if (length < 0 || length > srclength ||
			(index + length) > msblk->bytes_used)
		goto read_failure;

	for (b = 0; bytes < length; b++, cur_index++) {
		bh[b] = sb_getblk(sb, cur_index);
		if (bh[b] == NULL)
			goto block_release;
		bytes += msblk->devblksize;
	}

	ll_rw_block(READ, b, bh);
	*nr_bh = b;
	return 0;

block_release:
	for (; k < b; k++)
		put_bh(bh[k]);

read_failure:
	ERROR("squashfs_read_submit failed to read block 0x%llx\n",
					(unsigned long long) index);
	return -EIO;
}

/*
 * Read and decompress a datablock. @length should be non-zero(the size is
 * stored elsewhere in the filesystem). A bit in the length field indicates
 * if the block is stored uncompressed in the filesystem (usually because
 * compression generated a larger block - this does occasionally happen with
 * compression algorithms).
 */
int squashfs_read_datablock(struct super_block *sb, void **buffer, u64 index,
			int length, int srclength, int pages)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct buffer_head **bh;
	int offset = index & ((1 << msblk->devblksize_log2) - 1);
	int compressed, ret, b = 0;

	BUG_ON(!length);

	bh = kcalloc(((srclength + msblk->devblksize - 1)
		>> msblk->devblksize_log2) + 1, sizeof(*bh), GFP_KERNEL);
	if (bh == NULL)
		return -ENOMEM;

	compressed = SQUASHFS_COMPRESSED_BLOCK(length);
	length = SQUASHFS_COMPRESSED_SIZE_BLOCK(length);

	ret = squashfs_read_submit(sb, index, length, srclength, bh, &b);
	if (ret < 0) {
		kfree(bh);
		return ret;
	}


	TRACE("Data block @ 0x%llx, %scompressed size %d, src size %d\n",
		index, compressed ? "" : "un", length, srclength);

	length = squashfs_decompress_block(sb, compressed, buffer, bh,
				b, offset, length, srclength, pages);
	if (length < 0) {
		ERROR("squashfs_read_datablock failed to read block 0x%llx\n",
					(unsigned long long) index);
		kfree(bh);
		return -EIO;
	}

	kfree(bh);
	return length;
}

/*
 * Read and decompress a metadata block. @length is obtained from the first
 * two bytes of the metadata block.  A bit in the length field indicates if
 * the block is stored uncompressed in the filesystem (usually because
 * compression generated a larger block - this does occasionally happen with
 * compression algorithms).
 */
int squashfs_read_metablock(struct super_block *sb, void **buffer, u64 index,
			int length, u64 *next_index, int srclength, int pages)
{
	struct squashfs_sb_info *msblk = sb->s_fs_info;
	struct buffer_head **bh;
	int offset = index & ((1 << msblk->devblksize_log2) - 1);
	u64 cur_index = index >> msblk->devblksize_log2;
	int bytes, compressed, b = 0, k = 0;

	BUG_ON(length);

	bh = kcalloc(((srclength + msblk->devblksize - 1)
		>> msblk->devblksize_log2) + 1, sizeof(*bh), GFP_KERNEL);
	if (bh == NULL)
		return -ENOMEM;

	if ((index + 2) > msblk->bytes_used)
		goto read_failure;

	bh[0] = get_block_length(sb, &cur_index, &offset, &length);
	if (bh[0] == NULL)
		goto read_failure;
	b = 1;

	bytes = msblk->devblksize - offset;
	compressed = SQUASHFS_COMPRESSED(length);
	length = SQUASHFS_COMPRESSED_SIZE(length);
	if (next_index)
		*next_index = index + length + 2;

	TRACE("Meta block @ 0x%llx, %scompressed size %d\n", index,
			compressed ? "" : "un", length);

	if (length < 0 || length > srclength ||
				(index + length) > msblk->bytes_used)
		goto block_release;

	for (; bytes < length; b++) {
		bh[b] = sb_getblk(sb, ++cur_index);
		if (bh[b] == NULL)
			goto block_release;
		bytes += msblk->devblksize;
	}
	ll_rw_block(READ, b - 1, bh + 1);

	length = squashfs_decompress_block(sb, compressed, buffer, bh,
				b, offset, length, srclength, pages);
	if (length < 0)
		goto read_failure;

	kfree(bh);
	return length;

block_release:
	for (; k < b; k++)
		put_bh(bh[k]);

read_failure:
	ERROR("squashfs_read_metablock failed to read block 0x%llx\n",
					(unsigned long long) index);
	kfree(bh);
	return -EIO;
}
