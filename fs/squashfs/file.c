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
 * file.c
 */

/*
 * This file contains code for handling regular files.  A regular file
 * consists of a sequence of contiguous compressed blocks, and/or a
 * compressed fragment block (tail-end packed block).   The compressed size
 * of each datablock is stored in a block list contained within the
 * file inode (itself stored in one or more compressed metadata blocks).
 *
 * To speed up access to datablocks when reading 'large' files (256 Mbytes or
 * larger), the code implements an index cache that caches the mapping from
 * block index to datablock location on disk.
 *
 * The index cache allows Squashfs to handle large files (up to 1.75 TiB) while
 * retaining a simple and space-efficient block list on disk.  The cache
 * is split into slots, caching up to eight 224 GiB files (128 KiB blocks).
 * Larger files use multiple slots, with 1.75 TiB files using all 8 slots.
 * The index cache is designed to be memory efficient, and by default uses
 * 16 KiB.
 */

#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/mutex.h>
#include <linux/swap.h>

#include "squashfs_fs.h"
#include "squashfs_fs_sb.h"
#include "squashfs_fs_i.h"
#include "squashfs.h"

/*
 * Locate cache slot in range [offset, index] for specified inode.  If
 * there's more than one return the slot closest to index.
 */
static struct meta_index *locate_meta_index(struct inode *inode, int offset,
				int index)
{
	struct meta_index *meta = NULL;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int i;

	mutex_lock(&msblk->meta_index_mutex);

	TRACE("locate_meta_index: index %d, offset %d\n", index, offset);

	if (msblk->meta_index == NULL)
		goto not_allocated;

	for (i = 0; i < SQUASHFS_META_SLOTS; i++) {
		if (msblk->meta_index[i].inode_number == inode->i_ino &&
				msblk->meta_index[i].offset >= offset &&
				msblk->meta_index[i].offset <= index &&
				msblk->meta_index[i].locked == 0) {
			TRACE("locate_meta_index: entry %d, offset %d\n", i,
					msblk->meta_index[i].offset);
			meta = &msblk->meta_index[i];
			offset = meta->offset;
		}
	}

	if (meta)
		meta->locked = 1;

not_allocated:
	mutex_unlock(&msblk->meta_index_mutex);

	return meta;
}


/*
 * Find and initialise an empty cache slot for index offset.
 */
static struct meta_index *empty_meta_index(struct inode *inode, int offset,
				int skip)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	struct meta_index *meta = NULL;
	int i;

	mutex_lock(&msblk->meta_index_mutex);

	TRACE("empty_meta_index: offset %d, skip %d\n", offset, skip);

	if (msblk->meta_index == NULL) {
		/*
		 * First time cache index has been used, allocate and
		 * initialise.  The cache index could be allocated at
		 * mount time but doing it here means it is allocated only
		 * if a 'large' file is read.
		 */
		msblk->meta_index = kcalloc(SQUASHFS_META_SLOTS,
			sizeof(*(msblk->meta_index)), GFP_KERNEL);
		if (msblk->meta_index == NULL) {
			ERROR("Failed to allocate meta_index\n");
			goto failed;
		}
		for (i = 0; i < SQUASHFS_META_SLOTS; i++) {
			msblk->meta_index[i].inode_number = 0;
			msblk->meta_index[i].locked = 0;
		}
		msblk->next_meta_index = 0;
	}

	for (i = SQUASHFS_META_SLOTS; i &&
			msblk->meta_index[msblk->next_meta_index].locked; i--)
		msblk->next_meta_index = (msblk->next_meta_index + 1) %
			SQUASHFS_META_SLOTS;

	if (i == 0) {
		TRACE("empty_meta_index: failed!\n");
		goto failed;
	}

	TRACE("empty_meta_index: returned meta entry %d, %p\n",
			msblk->next_meta_index,
			&msblk->meta_index[msblk->next_meta_index]);

	meta = &msblk->meta_index[msblk->next_meta_index];
	msblk->next_meta_index = (msblk->next_meta_index + 1) %
			SQUASHFS_META_SLOTS;

	meta->inode_number = inode->i_ino;
	meta->offset = offset;
	meta->skip = skip;
	meta->entries = 0;
	meta->locked = 1;

failed:
	mutex_unlock(&msblk->meta_index_mutex);
	return meta;
}


static void release_meta_index(struct inode *inode, struct meta_index *meta)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	mutex_lock(&msblk->meta_index_mutex);
	meta->locked = 0;
	mutex_unlock(&msblk->meta_index_mutex);
}


/*
 * Read the next n blocks from the block list, starting from
 * metadata block <start_block, offset>.
 */
static long long read_indexes(struct super_block *sb, int n,
				u64 *start_block, int *offset)
{
	int err, i;
	long long block = 0;
	__le32 *blist = kmalloc(PAGE_CACHE_SIZE, GFP_KERNEL);

	if (blist == NULL) {
		ERROR("read_indexes: Failed to allocate block_list\n");
		return -ENOMEM;
	}

	while (n) {
		int blocks = min_t(int, n, PAGE_CACHE_SIZE >> 2);

		err = squashfs_read_metadata(sb, blist, start_block,
				offset, blocks << 2);
		if (err < 0) {
			ERROR("read_indexes: reading block [%llx:%x]\n",
				*start_block, *offset);
			goto failure;
		}

		for (i = 0; i < blocks; i++) {
			int size = le32_to_cpu(blist[i]);
			block += SQUASHFS_COMPRESSED_SIZE_BLOCK(size);
		}
		n -= blocks;
	}

	kfree(blist);
	return block;

failure:
	kfree(blist);
	return err;
}


/*
 * Each cache index slot has SQUASHFS_META_ENTRIES, each of which
 * can cache one index -> datablock/blocklist-block mapping.  We wish
 * to distribute these over the length of the file, entry[0] maps index x,
 * entry[1] maps index x + skip, entry[2] maps index x + 2 * skip, and so on.
 * The larger the file, the greater the skip factor.  The skip factor is
 * limited to the size of the metadata cache (SQUASHFS_CACHED_BLKS) to ensure
 * the number of metadata blocks that need to be read fits into the cache.
 * If the skip factor is limited in this way then the file will use multiple
 * slots.
 */
static inline int calculate_skip(int blocks)
{
	int skip = blocks / ((SQUASHFS_META_ENTRIES + 1)
		 * SQUASHFS_META_INDEXES);
	return min(SQUASHFS_CACHED_BLKS - 1, skip + 1);
}


/*
 * Search and grow the index cache for the specified inode, returning the
 * on-disk locations of the datablock and block list metadata block
 * <index_block, index_offset> for index (scaled to nearest cache index).
 */
static int fill_meta_index(struct inode *inode, int index,
		u64 *index_block, int *index_offset, u64 *data_block)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int skip = calculate_skip(i_size_read(inode) >> msblk->block_log);
	int offset = 0;
	struct meta_index *meta;
	struct meta_entry *meta_entry;
	u64 cur_index_block = squashfs_i(inode)->block_list_start;
	int cur_offset = squashfs_i(inode)->offset;
	u64 cur_data_block = squashfs_i(inode)->start;
	int err, i;

	/*
	 * Scale index to cache index (cache slot entry)
	 */
	index /= SQUASHFS_META_INDEXES * skip;

	while (offset < index) {
		meta = locate_meta_index(inode, offset + 1, index);

		if (meta == NULL) {
			meta = empty_meta_index(inode, offset + 1, skip);
			if (meta == NULL)
				goto all_done;
		} else {
			offset = index < meta->offset + meta->entries ? index :
				meta->offset + meta->entries - 1;
			meta_entry = &meta->meta_entry[offset - meta->offset];
			cur_index_block = meta_entry->index_block +
				msblk->inode_table;
			cur_offset = meta_entry->offset;
			cur_data_block = meta_entry->data_block;
			TRACE("get_meta_index: offset %d, meta->offset %d, "
				"meta->entries %d\n", offset, meta->offset,
				meta->entries);
			TRACE("get_meta_index: index_block 0x%llx, offset 0x%x"
				" data_block 0x%llx\n", cur_index_block,
				cur_offset, cur_data_block);
		}

		/*
		 * If necessary grow cache slot by reading block list.  Cache
		 * slot is extended up to index or to the end of the slot, in
		 * which case further slots will be used.
		 */
		for (i = meta->offset + meta->entries; i <= index &&
				i < meta->offset + SQUASHFS_META_ENTRIES; i++) {
			int blocks = skip * SQUASHFS_META_INDEXES;
			long long res = read_indexes(inode->i_sb, blocks,
					&cur_index_block, &cur_offset);

			if (res < 0) {
				if (meta->entries == 0)
					/*
					 * Don't leave an empty slot on read
					 * error allocated to this inode...
					 */
					meta->inode_number = 0;
				err = res;
				goto failed;
			}

			cur_data_block += res;
			meta_entry = &meta->meta_entry[i - meta->offset];
			meta_entry->index_block = cur_index_block -
				msblk->inode_table;
			meta_entry->offset = cur_offset;
			meta_entry->data_block = cur_data_block;
			meta->entries++;
			offset++;
		}

		TRACE("get_meta_index: meta->offset %d, meta->entries %d\n",
				meta->offset, meta->entries);

		release_meta_index(inode, meta);
	}

all_done:
	*index_block = cur_index_block;
	*index_offset = cur_offset;
	*data_block = cur_data_block;

	/*
	 * Scale cache index (cache slot entry) to index
	 */
	return offset * SQUASHFS_META_INDEXES * skip;

failed:
	release_meta_index(inode, meta);
	return err;
}


/*
 * Get the on-disk location and compressed size of the datablock
 * specified by index.  Fill_meta_index() does most of the work.
 */
static int read_blocklist(struct inode *inode, int index, u64 *block)
{
	u64 start;
	long long blks;
	int offset;
	__le32 size;
	int res = fill_meta_index(inode, index, &start, &offset, block);

	TRACE("read_blocklist: res %d, index %d, start 0x%llx, offset"
		       " 0x%x, block 0x%llx\n", res, index, start, offset,
			*block);

	if (res < 0)
		return res;

	/*
	 * res contains the index of the mapping returned by fill_meta_index(),
	 * this will likely be less than the desired index (because the
	 * meta_index cache works at a higher granularity).  Read any
	 * extra block indexes needed.
	 */
	if (res < index) {
		blks = read_indexes(inode->i_sb, index - res, &start, &offset);
		if (blks < 0)
			return (int) blks;
		*block += blks;
	}

	/*
	 * Read length of block specified by index.
	 */
	res = squashfs_read_metadata(inode->i_sb, &size, &start, &offset,
			sizeof(size));
	if (res < 0)
		return res;
	return le32_to_cpu(size);
}

static int squashfs_fragment_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int bytes, i, offset = 0;
	struct squashfs_cache_entry *buffer = NULL;
	void *pageaddr;

	int mask = (1 << (msblk->block_log - PAGE_CACHE_SHIFT)) - 1;
	int start_index = page->index & ~mask;

	TRACE("Entered squashfs_readpage, page index %lx, start block %llx\n",
				page->index, squashfs_i(inode)->start);

	/*
	 * Datablock is stored inside a fragment (tail-end packed
	 * block).
	 */
	buffer = squashfs_get_fragment(inode->i_sb,
			squashfs_i(inode)->fragment_block,
			squashfs_i(inode)->fragment_size);

	if (buffer->error) {
		ERROR("Unable to read page, block %llx, size %x\n",
			squashfs_i(inode)->fragment_block,
			squashfs_i(inode)->fragment_size);
		squashfs_cache_put(buffer);
		goto error_out;
	}

	bytes = i_size_read(inode) & (msblk->block_size - 1);
	offset = squashfs_i(inode)->fragment_offset;

	/*
	 * Loop copying datablock into pages.  As the datablock likely covers
	 * many PAGE_CACHE_SIZE pages (default block size is 128 KiB) explicitly
	 * grab the pages from the page cache, except for the page that we've
	 * been called to fill.
	 */
	for (i = start_index; bytes > 0; i++,
			bytes -= PAGE_CACHE_SIZE, offset += PAGE_CACHE_SIZE) {
		struct page *push_page;
		int avail = min_t(int, bytes, PAGE_CACHE_SIZE);

		TRACE("bytes %d, i %d, available_bytes %d\n", bytes, i, avail);

		push_page = (i == page->index) ? page :
			grab_cache_page_nowait(page->mapping, i);

		if (!push_page)
			continue;

		if (PageUptodate(push_page))
			goto skip_page;

		pageaddr = kmap_atomic(push_page);
		squashfs_copy_data(pageaddr, buffer, offset, avail);
		memset(pageaddr + avail, 0, PAGE_CACHE_SIZE - avail);
		kunmap_atomic(pageaddr);
		flush_dcache_page(push_page);
		SetPageUptodate(push_page);
skip_page:
		unlock_page(push_page);
		if (i != page->index)
			page_cache_release(push_page);
	}

	squashfs_cache_put(buffer);

	return 0;

error_out:
	SetPageError(page);
	pageaddr = kmap_atomic(page);
	memset(pageaddr, 0, PAGE_CACHE_SIZE);
	kunmap_atomic(pageaddr);
	flush_dcache_page(page);
	if (!PageError(page))
		SetPageUptodate(page);
	unlock_page(page);

	return 0;
}

static int squashfs_hole_readpages(struct inode *inode, int index,
				struct list_head *page_list)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int bytes, i, offset = 0;
	void *pageaddr;
	struct page *push_page;

	int start_index = index << (msblk->block_log - PAGE_CACHE_SHIFT);
	int file_end = i_size_read(inode) >> msblk->block_log;

	bytes = index == file_end ?
		(i_size_read(inode) & (msblk->block_size - 1)) :
		 msblk->block_size;

	/*
	 * Loop copying datablock into pages.  As the datablock likely covers
	 * many PAGE_CACHE_SIZE pages (default block size is 128 KiB) explicitly
	 * grab the pages from the page cache, except for the page that we've
	 * been called to fill.
	 */
	for (i = start_index; bytes > 0; i++,
			bytes -= PAGE_CACHE_SIZE, offset += PAGE_CACHE_SIZE) {

		push_page = list_entry(page_list->prev, struct page, lru);
		list_del(&push_page->lru);

		pageaddr = kmap_atomic(push_page);
		memset(pageaddr, 0, PAGE_CACHE_SIZE);
		kunmap_atomic(pageaddr);
		flush_dcache_page(push_page);
		SetPageUptodate(push_page);

		lru_cache_add_file(push_page);
		unlock_page(push_page);
		page_cache_release(push_page);
	}

	while (!list_empty(page_list)) {
		push_page = list_entry(page_list->prev, struct page, lru);
		list_del(&push_page->lru);
		page_cache_release(push_page);
	}

	return 0;
}

static int squashfs_hole_readpage(struct inode *inode, int index,
				struct page *page)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int bytes, i, offset = 0;
	void *pageaddr;

	int start_index = index << (msblk->block_log - PAGE_CACHE_SHIFT);
	int file_end = i_size_read(inode) >> msblk->block_log;

	bytes = index == file_end ?
		(i_size_read(inode) & (msblk->block_size - 1)) :
		 msblk->block_size;

	/*
	 * Loop copying datablock into pages.  As the datablock likely covers
	 * many PAGE_CACHE_SIZE pages (default block size is 128 KiB) explicitly
	 * grab the pages from the page cache, except for the page that we've
	 * been called to fill.
	 */
	for (i = start_index; bytes > 0; i++,
			bytes -= PAGE_CACHE_SIZE, offset += PAGE_CACHE_SIZE) {
		struct page *push_page;

		push_page = (i == page->index) ? page :
			grab_cache_page_nowait(inode->i_mapping, i);

		if (!push_page)
			continue;

		if (PageUptodate(push_page))
			goto skip_page;

		pageaddr = kmap_atomic(push_page);
		memset(pageaddr, 0, PAGE_CACHE_SIZE);
		kunmap_atomic(pageaddr);
		flush_dcache_page(push_page);
		SetPageUptodate(push_page);
skip_page:
		unlock_page(push_page);
		if (i == page->index)
			continue;
		page_cache_release(push_page);
	}

	return 0;
}

static int squashfs_regular_readpage(struct file *file, struct page *page)
{
	u64 block = 0;
	int bsize, i, data_len, pages, nr_pages = 0;
	struct address_space *mapping = page->mapping;
	struct inode *inode = mapping->host;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	gfp_t gfp_mask;
	struct page *push_page;
	struct page **page_array;
	void **buffer = NULL;
	void *pageaddr;

	int mask = (1 << (msblk->block_log - PAGE_CACHE_SHIFT)) - 1;
	int index = page->index >> (msblk->block_log - PAGE_CACHE_SHIFT);
	int start_index = page->index & ~mask;

	TRACE("Entered squashfs_readpage, page index %lx, start block %llx\n",
				page->index, squashfs_i(inode)->start);

	pages = msblk->block_size >> PAGE_CACHE_SHIFT;
	pages = pages ? pages : 1;
	/*
	 * Reading a datablock from disk.  Need to read block list
	 * to get location and block size.
	 */
	bsize = read_blocklist(inode, index, &block);
	if (bsize < 0)
		goto error_out;

	if (bsize == 0)
		return squashfs_hole_readpage(inode, index, page);

	/*
	 * Read and decompress data block
	 */
	gfp_mask = mapping_gfp_mask(mapping);
	buffer = kcalloc(1 << (msblk->block_log - PAGE_CACHE_SHIFT),
				sizeof(void *), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	page_array = kcalloc(1 << (msblk->block_log - PAGE_CACHE_SHIFT),
				sizeof(struct page *), GFP_KERNEL);

	if (!page_array)
		goto release_buffer;

	/* alloc buffer pages */
	for (i = 0; i < pages; i++) {
		if (page->index == start_index + i)
			push_page = page;
		else
			push_page = __page_cache_alloc(gfp_mask);
		if (!push_page)
			goto release_page_array;
		nr_pages++;
		buffer[i] = kmap(push_page);
		page_array[i] = push_page;
	}

	data_len = squashfs_read_datablock(inode->i_sb, buffer,
			block, bsize, msblk->block_size, pages);

	if (data_len < 0) {
		ERROR("Unable to read page, block %llx, size %x\n",
			block, bsize);
		for (i = 0; i < nr_pages; i++) {
			kunmap(page_array[i]);
			page_cache_release(page_array[i]);
		}
		kfree(buffer);
		kfree(page_array);
		goto error_out;
	}

	for (i = 0; i < pages; i++) {
		push_page = page_array[i];
		flush_dcache_page(push_page);
		SetPageUptodate(push_page);
		kunmap(page_array[i]);
		if (page->index == start_index + i) {
			unlock_page(push_page);
			continue;
		}

		if (add_to_page_cache_lru(push_page, mapping,
			start_index + i, gfp_mask)) {
			page_cache_release(push_page);
			continue;
		}

		unlock_page(push_page);
		page_cache_release(push_page);
	}

	kfree(page_array);
	kfree(buffer);
	return 0;

release_page_array:
	for (i = 0; i < nr_pages; i++) {
		kunmap(page_array[i]);
		page_cache_release(page_array[i]);
	}

	kfree(page_array);

release_buffer:
	kfree(buffer);
	return -ENOMEM;

error_out:
	SetPageError(page);
	pageaddr = kmap_atomic(page);
	memset(pageaddr, 0, PAGE_CACHE_SIZE);
	kunmap_atomic(pageaddr);
	flush_dcache_page(page);
	if (!PageError(page))
		SetPageUptodate(page);
	unlock_page(page);

	return 0;
}

static int squashfs_readpage(struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	void *pageaddr;

	int index = page->index >> (msblk->block_log - PAGE_CACHE_SHIFT);
	int file_end = i_size_read(inode) >> msblk->block_log;

	TRACE("Entered squashfs_readpage, page index %lx, start block %llx\n",
				page->index, squashfs_i(inode)->start);

	if (page->index >= ((i_size_read(inode) + PAGE_CACHE_SIZE - 1) >>
					PAGE_CACHE_SHIFT))
		goto out;

	if (index < file_end || squashfs_i(inode)->fragment_block ==
					SQUASHFS_INVALID_BLK)
		return squashfs_regular_readpage(file, page);
	else
		return squashfs_fragment_readpage(file, page);
out:
	pageaddr = kmap_atomic(page);
	memset(pageaddr, 0, PAGE_CACHE_SIZE);
	kunmap_atomic(pageaddr);
	flush_dcache_page(page);
	if (!PageError(page))
		SetPageUptodate(page);
	unlock_page(page);

	return 0;
}

static int squashfs_ra_readblock(struct inode *inode, int index,
			struct buffer_head **bh, int *nr_bh,
			int *block_size, u64 *block)
{
	int bsize;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int ret, b;

	/*
	 * Reading a datablock from disk.  Need to read block list
	 * to get location and block size.
	 */
	*block_size = read_blocklist(inode, index, block);
	if (*block_size < 0)
		return 1;

	if (*block_size == 0)
		return 0;

	bsize = SQUASHFS_COMPRESSED_SIZE_BLOCK(*block_size);
	ret = squashfs_read_submit(inode->i_sb, *block, bsize,
				msblk->block_size, bh, &b);
	if (ret < 0)
		return ret;

	*nr_bh = b;
	return 0;
}

struct squashfs_ra_private {
	struct buffer_head **bh;
	int nr_bh;
	int block_size;
	u64 block;
	void **buffer;
	struct page **page_array;
};

/* Caller should free buffer head */
static int squashfs_ra_read_submit(struct inode *inode, int bindex,
			struct squashfs_ra_private *ra_priv)
{
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int ret;
	struct buffer_head **bh;
	int nr_bh = -1, bsize;
	u64 block;
	bh = kcalloc(((msblk->block_size + msblk->devblksize - 1)
				>> msblk->devblksize_log2) + 1,
				sizeof(*bh), GFP_KERNEL);
	if (!bh)
		return -ENOMEM;

	ra_priv->bh = bh;
	ret = squashfs_ra_readblock(inode, bindex, bh, &nr_bh,
					&bsize, &block);
	if (ret != 0)
		goto release_bh;

	ra_priv->nr_bh = nr_bh;
	ra_priv->block_size = bsize;
	ra_priv->block = block;

	return 0;

release_bh:
	kfree(ra_priv->bh);
	return ret;
}

static int squashfs_ra_decompress(struct inode *inode, int bindex,
			struct squashfs_ra_private *ra_priv, gfp_t gfp_mask,
			struct list_head *page_list)
{
	int j;
	int ret = -ENOMEM;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	int pages_per_block;

	void **buffer;
	struct buffer_head **bh;
	int block_size, nr_bh;
	u64 block;
	struct page **page_array, *page;

	int compressed, offset, length;

	pages_per_block = msblk->block_size >> PAGE_CACHE_SHIFT;
	pages_per_block = pages_per_block ? pages_per_block : 1;

	bh = ra_priv->bh;
	block_size = ra_priv->block_size;
	nr_bh = ra_priv->nr_bh;
	block = ra_priv->block;

	if (block_size == 0)
		return squashfs_hole_readpages(inode, bindex, page_list);

	buffer = kcalloc(1 << (msblk->block_log - PAGE_CACHE_SHIFT),
					sizeof(void *), GFP_KERNEL);
	if (!buffer)
		goto out;
	ra_priv->buffer = buffer;

	page_array = kcalloc(1 << (msblk->block_log - PAGE_CACHE_SHIFT),
					sizeof(struct page *), GFP_KERNEL);
	if (!page_array)
		goto release_buffer;
	ra_priv->page_array = page_array;

	/* alloc buffer pages */
	for (j = 0; j < pages_per_block; j++) {
		page = list_entry(page_list->prev, struct page, lru);
		list_del(&page->lru);
		buffer[j] = kmap(page);
		page_array[j] = page;
	}

	compressed = SQUASHFS_COMPRESSED_BLOCK(block_size);
	length = SQUASHFS_COMPRESSED_SIZE_BLOCK(block_size);

	offset = block & ((1 << msblk->devblksize_log2) - 1);
	length = squashfs_decompress_block(inode->i_sb, compressed, buffer,
				bh, nr_bh, offset, length, msblk->block_size,
				pages_per_block);

	for (j = 0; j < pages_per_block; j++) {
		page = page_array[j];
		flush_dcache_page(page);
		SetPageUptodate(page);
		lru_cache_add_file(page);
		if (!PageLocked(page))
			printk("unlock page %p j %d\n", page, j);
		unlock_page(page);
	}

	ret = 0;

release_buffer:
	if (ra_priv->buffer) {
		for (j = 0; j < pages_per_block; j++) {
			if (!ra_priv->buffer[j])
				break;
			kunmap(ra_priv->page_array[j]);
			page_cache_release(ra_priv->page_array[j]);
		}
		kfree(ra_priv->page_array);
	}

	kfree(ra_priv->buffer);
out:
	return ret;
}

/*
 * Fill hole pages between page_index and last_page_index
 * Return 0 if function is successful, otherwise, return 1
 * All pages are locked and added into page cache so that caller should
 * add them into LRU and unlock.
 */
int hole_plugging(struct squashfs_sb_info *msblk, struct list_head *page_list,
				int start_bindex, int last_bindex,
				struct address_space *mapping)
{
	struct page *page, *tmp_page, *hpage, *tpage;
	int page_index, last_page_index;
	gfp_t gfp_mask = mapping_gfp_mask(mapping);

	page_index = start_bindex << (msblk->block_log - PAGE_CACHE_SHIFT);
	last_page_index = ((last_bindex + 1) <<
			(msblk->block_log - PAGE_CACHE_SHIFT));
	
	hpage = list_entry(page_list->prev, struct page, lru);
	for (; page_index < hpage->index; page_index++) {
		struct page *new_page = page_cache_alloc_readahead(mapping);
		if (!new_page)
			return 1;
		new_page->index = page_index;
		list_add(&new_page->lru, &hpage->lru);
	}

	tpage = list_entry(page_list->next, struct page, lru);
	page_index = tpage->index + 1;
	for (; page_index < last_page_index; page_index++) {
		struct page *new_page = page_cache_alloc_readahead(mapping);
		if (!new_page)
			return 1;
		new_page->index = page_index;
		list_add(&new_page->lru, page_list);
	}

	list_for_each_entry_reverse(page, page_list, lru)
		if (add_to_page_cache(page, mapping, page->index, gfp_mask))
			goto remove_pagecache;
	return 0;

remove_pagecache:
	list_for_each_entry_reverse(tmp_page, page_list, lru) {
		if (tmp_page == page)
			break;
		delete_from_page_cache(tmp_page);
		unlock_page(tmp_page);
	}
	return 1;
}

struct decomp_work {
	struct inode *inode;
	int bindex;
	struct list_head list;
	struct list_head pages;
	struct squashfs_ra_private *ra_priv;
	gfp_t gfp_mask;
};

void squashfs_decomp_work(struct work_struct *work)
{
	struct inode *inode;
	struct list_head *pages;
	struct squashfs_ra_private *ra_priv;
	struct decomp_work *decomp_work;
	gfp_t gfp_mask;
	int bindex;
	struct squashfs_sb_info *msblk =
		container_of(work, struct squashfs_sb_info, delay_work.work);

	for (;;) {
		decomp_work = NULL;
		spin_lock(&msblk->decomp_lock);
		if (!list_empty(&msblk->decomp_list)) {
			decomp_work = list_entry(msblk->decomp_list.prev,
					struct decomp_work, list);
			list_del(&decomp_work->list);
		}
		spin_unlock(&msblk->decomp_lock);
		if (!decomp_work)
			return;

		inode = decomp_work->inode;
		bindex = decomp_work->bindex;
		ra_priv = decomp_work->ra_priv;
		gfp_mask = decomp_work->gfp_mask;
		pages = &decomp_work->pages;

		if (squashfs_ra_decompress(inode, bindex, ra_priv,
						gfp_mask, pages)) {
			struct page *page, *tmp;
			list_for_each_entry_safe_reverse(page, tmp,
							pages, lru) {
				list_del(&page->lru);
				delete_from_page_cache(page);
				unlock_page(page);
				page_cache_release(page);
			}
		}

		kfree(ra_priv->bh);
		kfree(ra_priv);
		kfree(decomp_work);
	}
}

static int move_pages(struct list_head *page_list, struct list_head *pages,
				int nr)
{
	int moved_pages = 0;
	struct page *page, *tmp_page;
	list_for_each_entry_safe_reverse(page, tmp_page,
						page_list, lru) {
		list_move(&page->lru, pages);
		moved_pages++;
		if (moved_pages == nr)
			break;
	}

	return moved_pages;
}

static int squashfs_readpages(struct file *file, struct address_space *mapping,
		struct list_head *page_list, unsigned nr_pages)
{
	struct inode *inode = mapping->host;
	struct squashfs_sb_info *msblk = inode->i_sb->s_fs_info;
	struct page *hpage, *tpage, *page;
	struct decomp_work **work;
	int start_bindex, last_bindex;
	struct squashfs_ra_private **ra_priv;
	int pages_per_block, i, ret = -ENOMEM;
	gfp_t gfp_mask;
	int nr_blocks;

	gfp_mask = mapping_gfp_mask(mapping);

	hpage = list_entry(page_list->prev, struct page, lru);
	tpage = list_entry(page_list->next, struct page, lru);

	start_bindex = hpage->index >> (msblk->block_log - PAGE_CACHE_SHIFT);
	last_bindex = tpage->index >> (msblk->block_log - PAGE_CACHE_SHIFT);

	/*
	 * Normally, MM readahread window is smaller than our compressed
	 * block size. In that case, plugging could hurt performance so
	 * let's do synchronous read in that case.
	 */
	if (start_bindex == last_bindex) {
		list_del(&hpage->lru);
		if (add_to_page_cache_lru(hpage, mapping, hpage->index,
				GFP_KERNEL)) {
			page_cache_release(hpage);
			return 0;
		}

		ret = squashfs_readpage(file, hpage);
		page_cache_release(hpage);
		return ret;
	}

	if (last_bindex >= (i_size_read(inode) >> msblk->block_log))
		return 0;

	/* fill with pages for readahead  */
	if (hole_plugging(msblk, page_list, start_bindex,
					last_bindex, mapping)) {
		ret = 0;
		goto out;
	}

	nr_blocks = last_bindex - start_bindex + 1;
	ra_priv = kcalloc(nr_blocks, sizeof(*ra_priv), GFP_KERNEL);
	if (!ra_priv)
		goto remove_pagecache;

	for (i = 0; i < nr_blocks; i++) {
		ra_priv[i] = kmalloc(sizeof(**ra_priv),	GFP_KERNEL);
		if (ra_priv[i] == NULL)
			goto release_ra_priv;
	}

	work = kcalloc(nr_blocks, sizeof(*work), GFP_KERNEL);
	if (!work)
		goto release_ra_priv;

	for (i = 0; i < nr_blocks; i++) {
		work[i] = kmalloc(sizeof(**work), GFP_KERNEL);
		if (!work[i])
			goto release_work;
	}

	for (i = 0; i < nr_blocks; i++) {
		ret = squashfs_ra_read_submit(inode, start_bindex + i ,
							ra_priv[i]);
		if (ret)
			goto release_ra_priv;
	}

	ret = 0;

	queue_delayed_work(system_unbound_wq, &msblk->delay_work,
		msecs_to_jiffies(3));

	pages_per_block = msblk->block_size >> PAGE_CACHE_SHIFT;
	pages_per_block = pages_per_block ? pages_per_block : 1;

	for (i = 0; i < nr_blocks; i++) {
		struct decomp_work *decomp_work = work[i];

		INIT_LIST_HEAD(&decomp_work->pages);
		decomp_work->bindex = start_bindex + i;
		decomp_work->ra_priv = ra_priv[i];
		decomp_work->gfp_mask = gfp_mask;
		decomp_work->inode = inode;

		move_pages(page_list, &decomp_work->pages,
			pages_per_block);

		spin_lock(&msblk->decomp_lock);
		list_add(&decomp_work->list, &msblk->decomp_list);
		spin_unlock(&msblk->decomp_lock);
	}

	kfree(ra_priv);
	kfree(work);

	return ret;

release_work:
	for (i = 0; i < nr_blocks; i++)
		kfree(work[i]);
	kfree(work);
release_ra_priv:
	for (i = 0; i < nr_blocks; i++)
		kfree(ra_priv[i]);
	kfree(ra_priv);
remove_pagecache:
	list_for_each_entry_reverse(page, page_list, lru) {
		delete_from_page_cache(page);
		unlock_page(page);
	}
out:
	return ret;
}

const struct address_space_operations squashfs_aops = {
	.readpage = squashfs_readpage,
	.readpages = squashfs_readpages,
};
