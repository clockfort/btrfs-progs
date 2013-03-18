/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <zlib.h>
#include "kerncompat.h"
#include "crc32c.h"
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "utils.h"
#include "version.h"


#define HEADER_MAGIC		0xbd5c25e27295668bULL
#define MAX_PENDING_SIZE	(256 * 1024)
#define BLOCK_SIZE		1024
#define BLOCK_MASK		(BLOCK_SIZE - 1)

#define COMPRESS_NONE		0
#define COMPRESS_ZLIB		1

struct meta_cluster_item {
	__le64 bytenr;
	__le32 size;
} __attribute__ ((__packed__));

struct meta_cluster_header {
	__le64 magic;
	__le64 bytenr;
	__le32 nritems;
	u8 compress;
} __attribute__ ((__packed__));

/* cluster header + index items + buffers */
struct meta_cluster {
	struct meta_cluster_header header;
	struct meta_cluster_item items[];
} __attribute__ ((__packed__));

#define ITEMS_PER_CLUSTER ((BLOCK_SIZE - sizeof(struct meta_cluster)) / \
			   sizeof(struct meta_cluster_item))

struct async_work {
	struct list_head list;
	struct list_head ordered;
	u64 start;
	u64 size;
	u8 *buffer;
	size_t bufsize;
	int error;
};

struct metadump_struct {
	struct btrfs_root *root;
	FILE *out;

	struct meta_cluster *cluster;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct list_head list;
	struct list_head ordered;
	size_t num_items;
	size_t num_ready;

	u64 pending_start;
	u64 pending_size;

	int compress_level;
	int done;
};

struct mdrestore_struct {
	FILE *in;
	FILE *out;

	pthread_t *threads;
	size_t num_threads;
	pthread_mutex_t mutex;
	pthread_cond_t cond;

	struct list_head list;
	size_t num_items;

	int compress_method;
	int done;
	int error;
};

static void csum_block(u8 *buf, size_t len)
{
	char result[BTRFS_CRC32_SIZE];
	u32 crc = ~(u32)0;
	crc = crc32c(crc, buf + BTRFS_CSUM_SIZE, len - BTRFS_CSUM_SIZE);
	btrfs_csum_final(crc, result);
	memcpy(buf, result, BTRFS_CRC32_SIZE);
}

/*
 * zero inline extents and csum items
 */
static void zero_items(u8 *dst, struct extent_buffer *src)
{
	struct btrfs_file_extent_item *fi;
	struct btrfs_item *item;
	struct btrfs_key key;
	u32 nritems = btrfs_header_nritems(src);
	size_t size;
	unsigned long ptr;
	int i, extent_type;

	for (i = 0; i < nritems; i++) {
		item = btrfs_item_nr(src, i);
		btrfs_item_key_to_cpu(src, &key, i);
		if (key.type == BTRFS_CSUM_ITEM_KEY) {
			size = btrfs_item_size_nr(src, i);
			memset(dst + btrfs_leaf_data(src) +
			       btrfs_item_offset_nr(src, i), 0, size);
			continue;
		}
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;

		fi = btrfs_item_ptr(src, i, struct btrfs_file_extent_item);
		extent_type = btrfs_file_extent_type(src, fi);
		if (extent_type != BTRFS_FILE_EXTENT_INLINE)
			continue;

		ptr = btrfs_file_extent_inline_start(fi);
		size = btrfs_file_extent_inline_item_len(src, item);
		memset(dst + ptr, 0, size);
	}
}

/*
 * copy buffer and zero useless data in the buffer
 */
static void copy_buffer(u8 *dst, struct extent_buffer *src)
{
	int level;
	size_t size;
	u32 nritems;

	memcpy(dst, src->data, src->len);
	if (src->start == BTRFS_SUPER_INFO_OFFSET)
		return;

	level = btrfs_header_level(src);
	nritems = btrfs_header_nritems(src);

	if (nritems == 0) {
		size = sizeof(struct btrfs_header);
		memset(dst + size, 0, src->len - size);
	} else if (level == 0) {
		size = btrfs_leaf_data(src) +
			btrfs_item_offset_nr(src, nritems - 1) -
			btrfs_item_nr_offset(nritems);
		memset(dst + btrfs_item_nr_offset(nritems), 0, size);
		zero_items(dst, src);
	} else {
		size = offsetof(struct btrfs_node, ptrs) +
			sizeof(struct btrfs_key_ptr) * nritems;
		memset(dst + size, 0, src->len - size);
	}
	csum_block(dst, src->len);
}

static void *dump_worker(void *data)
{
	struct metadump_struct *md = (struct metadump_struct *)data;
	struct async_work *async;
	int ret;

	while (1) {
		pthread_mutex_lock(&md->mutex);
		while (list_empty(&md->list)) {
			if (md->done) {
				pthread_mutex_unlock(&md->mutex);
				goto out;
			}
			pthread_cond_wait(&md->cond, &md->mutex);
		}
		async = list_entry(md->list.next, struct async_work, list);
		list_del_init(&async->list);
		pthread_mutex_unlock(&md->mutex);

		if (md->compress_level > 0) {
			u8 *orig = async->buffer;

			async->bufsize = compressBound(async->size);
			async->buffer = malloc(async->bufsize);

			ret = compress2(async->buffer,
					 (unsigned long *)&async->bufsize,
					 orig, async->size, md->compress_level);

			if (ret != Z_OK)
				async->error = 1;

			free(orig);
		}

		pthread_mutex_lock(&md->mutex);
		md->num_ready++;
		pthread_mutex_unlock(&md->mutex);
	}
out:
	pthread_exit(NULL);
}

static void meta_cluster_init(struct metadump_struct *md, u64 start)
{
	struct meta_cluster_header *header;

	md->num_items = 0;
	md->num_ready = 0;
	header = &md->cluster->header;
	header->magic = cpu_to_le64(HEADER_MAGIC);
	header->bytenr = cpu_to_le64(start);
	header->nritems = cpu_to_le32(0);
	header->compress = md->compress_level > 0 ?
			   COMPRESS_ZLIB : COMPRESS_NONE;
}

static int metadump_init(struct metadump_struct *md, struct btrfs_root *root,
			 FILE *out, int num_threads, int compress_level)
{
	int i, ret;

	memset(md, 0, sizeof(*md));
	pthread_cond_init(&md->cond, NULL);
	pthread_mutex_init(&md->mutex, NULL);
	INIT_LIST_HEAD(&md->list);
	INIT_LIST_HEAD(&md->ordered);
	md->root = root;
	md->out = out;
	md->pending_start = (u64)-1;
	md->compress_level = compress_level;
	md->cluster = calloc(1, BLOCK_SIZE);
	if (!md->cluster) {
		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		return -ENOMEM;
	}

	meta_cluster_init(md, 0);
	if (!num_threads)
		return 0;

	md->num_threads = num_threads;
	md->threads = calloc(num_threads, sizeof(pthread_t));
	if (!md->threads) {
		free(md->cluster);
		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		return -ENOMEM;
	}

	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(md->threads + i, NULL, dump_worker, md);
		if (ret)
			break;
	}

	if (ret) {
		pthread_mutex_lock(&md->mutex);
		md->done = 1;
		pthread_cond_broadcast(&md->cond);
		pthread_mutex_unlock(&md->mutex);

		for (i--; i >= 0; i--)
			pthread_join(md->threads[i], NULL);

		pthread_cond_destroy(&md->cond);
		pthread_mutex_destroy(&md->mutex);
		free(md->cluster);
		free(md->threads);
	}

	return ret;
}

static void metadump_destroy(struct metadump_struct *md)
{
	int i;
	pthread_mutex_lock(&md->mutex);
	md->done = 1;
	pthread_cond_broadcast(&md->cond);
	pthread_mutex_unlock(&md->mutex);

	for (i = 0; i < md->num_threads; i++)
		pthread_join(md->threads[i], NULL);

	pthread_cond_destroy(&md->cond);
	pthread_mutex_destroy(&md->mutex);
	free(md->threads);
	free(md->cluster);
}

static int write_zero(FILE *out, size_t size)
{
	static char zero[BLOCK_SIZE];
	return fwrite(zero, size, 1, out);
}

static int write_buffers(struct metadump_struct *md, u64 *next)
{
	struct meta_cluster_header *header = &md->cluster->header;
	struct meta_cluster_item *item;
	struct async_work *async;
	u64 bytenr = 0;
	u32 nritems = 0;
	int ret;
	int err = 0;

	if (list_empty(&md->ordered))
		goto out;

	/* wait until all buffers are compressed */
	while (md->num_items > md->num_ready) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&md->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&md->mutex);
	}

	/* setup and write index block */
	list_for_each_entry(async, &md->ordered, ordered) {
		item = md->cluster->items + nritems;
		item->bytenr = cpu_to_le64(async->start);
		item->size = cpu_to_le32(async->bufsize);
		nritems++;
	}
	header->nritems = cpu_to_le32(nritems);

	ret = fwrite(md->cluster, BLOCK_SIZE, 1, md->out);
	if (ret != 1) {
		fprintf(stderr, "Error writing out cluster: %d\n", errno);
		return -EIO;
	}

	/* write buffers */
	bytenr += le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	while (!list_empty(&md->ordered)) {
		async = list_entry(md->ordered.next, struct async_work,
				   ordered);
		list_del_init(&async->ordered);

		bytenr += async->bufsize;
		if (!err)
			ret = fwrite(async->buffer, async->bufsize, 1,
				     md->out);
		if (ret != 1) {
			err = -EIO;
			ret = 0;
			fprintf(stderr, "Error writing out cluster: %d\n",
				errno);
		}

		free(async->buffer);
		free(async);
	}

	/* zero unused space in the last block */
	if (!err && bytenr & BLOCK_MASK) {
		size_t size = BLOCK_SIZE - (bytenr & BLOCK_MASK);

		bytenr += size;
		ret = write_zero(md->out, size);
		if (ret != 1) {
			fprintf(stderr, "Error zeroing out buffer: %d\n",
				errno);
			err = -EIO;
		}
	}
out:
	*next = bytenr;
	return err;
}

static int flush_pending(struct metadump_struct *md, int done)
{
	struct async_work *async = NULL;
	struct extent_buffer *eb;
	u64 blocksize = md->root->nodesize;
	u64 start;
	u64 size;
	size_t offset;
	int ret = 0;

	if (md->pending_size) {
		async = calloc(1, sizeof(*async));
		if (!async)
			return -ENOMEM;

		async->start = md->pending_start;
		async->size = md->pending_size;
		async->bufsize = async->size;
		async->buffer = malloc(async->bufsize);
		if (!async->buffer) {
			free(async);
			return -ENOMEM;
		}
		offset = 0;
		start = async->start;
		size = async->size;
		while (size > 0) {
			eb = read_tree_block(md->root, start, blocksize, 0);
			if (!eb) {
				free(async->buffer);
				free(async);
				fprintf(stderr, "Error reading metadata "
					"block\n");
				return -EIO;
			}
			copy_buffer(async->buffer + offset, eb);
			free_extent_buffer(eb);
			start += blocksize;
			offset += blocksize;
			size -= blocksize;
		}

		md->pending_start = (u64)-1;
		md->pending_size = 0;
	} else if (!done) {
		return 0;
	}

	pthread_mutex_lock(&md->mutex);
	if (async) {
		list_add_tail(&async->ordered, &md->ordered);
		md->num_items++;
		if (md->compress_level > 0) {
			list_add_tail(&async->list, &md->list);
			pthread_cond_signal(&md->cond);
		} else {
			md->num_ready++;
		}
	}
	if (md->num_items >= ITEMS_PER_CLUSTER || done) {
		ret = write_buffers(md, &start);
		if (ret)
			fprintf(stderr, "Error writing buffers %d\n",
				errno);
		else
			meta_cluster_init(md, start);
	}
	pthread_mutex_unlock(&md->mutex);
	return ret;
}

static int add_metadata(u64 start, u64 size, struct metadump_struct *md)
{
	int ret;
	if (md->pending_size + size > MAX_PENDING_SIZE ||
	    md->pending_start + md->pending_size != start) {
		ret = flush_pending(md, 0);
		if (ret)
			return ret;
		md->pending_start = start;
	}
	readahead_tree_block(md->root, start, size, 0);
	md->pending_size += size;
	return 0;
}

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static int is_tree_block(struct btrfs_root *extent_root,
			 struct btrfs_path *path, u64 bytenr)
{
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 ref_objectid;
	int ret;

	leaf = path->nodes[0];
	while (1) {
		struct btrfs_extent_ref_v0 *ref_item;
		path->slots[0]++;
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0)
				return ret;
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid != bytenr)
			break;
		if (key.type != BTRFS_EXTENT_REF_V0_KEY)
			continue;
		ref_item = btrfs_item_ptr(leaf, path->slots[0],
					  struct btrfs_extent_ref_v0);
		ref_objectid = btrfs_ref_objectid_v0(leaf, ref_item);
		if (ref_objectid < BTRFS_FIRST_FREE_OBJECTID)
			return 1;
		break;
	}
	return 0;
}
#endif

static int copy_log_blocks(struct btrfs_root *root, struct extent_buffer *eb,
			   struct metadump_struct *metadump,
			   int log_root_tree)
{
	struct extent_buffer *tmp;
	struct btrfs_root_item *ri;
	struct btrfs_key key;
	u64 bytenr;
	int level;
	int nritems = 0;
	int i = 0;
	int ret;

	ret = add_metadata(btrfs_header_bytenr(eb), root->leafsize, metadump);
	if (ret) {
		fprintf(stderr, "Error adding metadata block\n");
		return ret;
	}

	if (btrfs_header_level(eb) == 0 && !log_root_tree)
		return 0;

	level = btrfs_header_level(eb);
	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		if (level == 0) {
			btrfs_item_key_to_cpu(eb, &key, i);
			if (key.type != BTRFS_ROOT_ITEM_KEY)
				continue;
			ri = btrfs_item_ptr(eb, i, struct btrfs_root_item);
			bytenr = btrfs_disk_root_bytenr(eb, ri);
			tmp = read_tree_block(root, bytenr, root->leafsize, 0);
			if (!tmp) {
				fprintf(stderr, "Error reading log root "
					"block\n");
				return -EIO;
			}
			ret = copy_log_blocks(root, tmp, metadump, 0);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		} else {
			bytenr = btrfs_node_blockptr(eb, i);
			tmp = read_tree_block(root, bytenr, root->leafsize, 0);
			if (!tmp) {
				fprintf(stderr, "Error reading log block\n");
				return -EIO;
			}
			ret = copy_log_blocks(root, tmp, metadump,
					      log_root_tree);
			free_extent_buffer(tmp);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int copy_log_trees(struct btrfs_root *root,
			  struct metadump_struct *metadump,
			  struct btrfs_path *path)
{
	u64 blocknr = btrfs_super_log_root(&root->fs_info->super_copy);

	if (blocknr == 0)
		return 0;

	if (!root->fs_info->log_root_tree ||
	    !root->fs_info->log_root_tree->node) {
		fprintf(stderr, "Error copying tree log, it wasn't setup\n");
		return -EIO;
	}

	return copy_log_blocks(root, root->fs_info->log_root_tree->node,
			       metadump, 1);
}

static int create_metadump(const char *input, FILE *out, int num_threads,
			   int compress_level)
{
	struct btrfs_root *root;
	struct btrfs_root *extent_root;
	struct btrfs_path *path = NULL;
	struct extent_buffer *leaf;
	struct btrfs_extent_item *ei;
	struct btrfs_key key;
	struct metadump_struct metadump;
	u64 bytenr;
	u64 num_bytes;
	int ret;
	int err = 0;

	root = open_ctree(input, 0, 0);
	if (!root) {
		fprintf(stderr, "Open ctree failed\n");
		return -EIO;
	}

	BUG_ON(root->nodesize != root->leafsize);

	ret = metadump_init(&metadump, root, out, num_threads,
			    compress_level);
	if (ret) {
		fprintf(stderr, "Error initing metadump %d\n", ret);
		close_ctree(root);
		return ret;
	}

	ret = add_metadata(BTRFS_SUPER_INFO_OFFSET, 4096, &metadump);
	if (ret) {
		fprintf(stderr, "Error adding metadata %d\n", ret);
		err = ret;
		goto out;
	}

	extent_root = root->fs_info->extent_root;
	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Out of memory allocing path\n");
		err = -ENOMEM;
		goto out;
	}
	bytenr = BTRFS_SUPER_INFO_OFFSET + 4096;
	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, extent_root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching extent root %d\n", ret);
		err = ret;
		goto out;
	}

	while (1) {
		leaf = path->nodes[0];
		if (path->slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf %d"
					"\n", ret);
				err = ret;
				goto out;
			}
			if (ret > 0)
				break;
			leaf = path->nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid < bytenr ||
		    (key.type != BTRFS_EXTENT_ITEM_KEY &&
		     key.type != BTRFS_METADATA_ITEM_KEY)) {
			path->slots[0]++;
			continue;
		}

		bytenr = key.objectid;
		if (key.type == BTRFS_METADATA_ITEM_KEY)
			num_bytes = key.offset;
		else
			num_bytes = root->leafsize;

		if (btrfs_item_size_nr(leaf, path->slots[0]) > sizeof(*ei)) {
			ei = btrfs_item_ptr(leaf, path->slots[0],
					    struct btrfs_extent_item);
			if (btrfs_extent_flags(leaf, ei) &
			    BTRFS_EXTENT_FLAG_TREE_BLOCK) {
				ret = add_metadata(bytenr, num_bytes,
						   &metadump);
				if (ret) {
					fprintf(stderr, "Error adding block "
						"%d\n", ret);
					err = ret;
					goto out;
				}
			}
		} else {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
			ret = is_tree_block(extent_root, path, bytenr);
			if (ret < 0) {
				fprintf(stderr, "Error checking tree block "
					"%d\n", ret);
				err = ret;
				goto out;
			}

			if (ret) {
				ret = add_metadata(bytenr, num_bytes,
						   &metadump);
				if (ret) {
					fprintf(stderr, "Error adding block "
						"%d\n", ret);
					err = ret;
					goto out;
				}
			}
#else
			fprintf(stderr, "Either extent tree corruption or "
				"you haven't built with V0 support\n");
			err = -EIO;
			goto out;
#endif
		}
		bytenr += num_bytes;
	}

	ret = copy_log_trees(root, &metadump, path);
	if (ret)
		err = ret;
out:
	ret = flush_pending(&metadump, 1);
	if (ret) {
		if (!err)
			ret = err;
		fprintf(stderr, "Error flushing pending %d\n", ret);
	}

	metadump_destroy(&metadump);

	btrfs_free_path(path);
	ret = close_ctree(root);
	return err ? err : ret;
}

static void update_super(u8 *buffer)
{
	struct btrfs_super_block *super = (struct btrfs_super_block *)buffer;
	struct btrfs_chunk *chunk;
	struct btrfs_disk_key *key;
	u32 sectorsize = btrfs_super_sectorsize(super);
	u64 flags = btrfs_super_flags(super);

	flags |= BTRFS_SUPER_FLAG_METADUMP;
	btrfs_set_super_flags(super, flags);

	key = (struct btrfs_disk_key *)(super->sys_chunk_array);
	chunk = (struct btrfs_chunk *)(super->sys_chunk_array +
				       sizeof(struct btrfs_disk_key));

	btrfs_set_disk_key_objectid(key, BTRFS_FIRST_CHUNK_TREE_OBJECTID);
	btrfs_set_disk_key_type(key, BTRFS_CHUNK_ITEM_KEY);
	btrfs_set_disk_key_offset(key, 0);

	btrfs_set_stack_chunk_length(chunk, (u64)-1);
	btrfs_set_stack_chunk_owner(chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_stack_chunk_stripe_len(chunk, 64 * 1024);
	btrfs_set_stack_chunk_type(chunk, BTRFS_BLOCK_GROUP_SYSTEM);
	btrfs_set_stack_chunk_io_align(chunk, sectorsize);
	btrfs_set_stack_chunk_io_width(chunk, sectorsize);
	btrfs_set_stack_chunk_sector_size(chunk, sectorsize);
	btrfs_set_stack_chunk_num_stripes(chunk, 1);
	btrfs_set_stack_chunk_sub_stripes(chunk, 0);
	chunk->stripe.devid = super->dev_item.devid;
	chunk->stripe.offset = cpu_to_le64(0);
	memcpy(chunk->stripe.dev_uuid, super->dev_item.uuid, BTRFS_UUID_SIZE);
	btrfs_set_super_sys_array_size(super, sizeof(*key) + sizeof(*chunk));
	csum_block(buffer, 4096);
}

static void *restore_worker(void *data)
{
	struct mdrestore_struct *mdres = (struct mdrestore_struct *)data;
	struct async_work *async;
	size_t size;
	u8 *buffer;
	u8 *outbuf;
	int outfd;
	int ret;

	outfd = fileno(mdres->out);
	buffer = malloc(MAX_PENDING_SIZE * 2);
	if (!buffer) {
		fprintf(stderr, "Error allocing buffer\n");
		pthread_mutex_lock(&mdres->mutex);
		if (!mdres->error)
			mdres->error = -ENOMEM;
		pthread_mutex_unlock(&mdres->mutex);
		goto out;
	}

	while (1) {
		int err = 0;

		pthread_mutex_lock(&mdres->mutex);
		while (list_empty(&mdres->list)) {
			if (mdres->done) {
				pthread_mutex_unlock(&mdres->mutex);
				goto out;
			}
			pthread_cond_wait(&mdres->cond, &mdres->mutex);
		}
		async = list_entry(mdres->list.next, struct async_work, list);
		list_del_init(&async->list);
		pthread_mutex_unlock(&mdres->mutex);

		if (mdres->compress_method == COMPRESS_ZLIB) {
			size = MAX_PENDING_SIZE * 2;
			ret = uncompress(buffer, (unsigned long *)&size,
					 async->buffer, async->bufsize);
			if (ret != Z_OK) {
				fprintf(stderr, "Error decompressing %d\n",
					ret);
				err = -EIO;
			}
			outbuf = buffer;
		} else {
			outbuf = async->buffer;
			size = async->bufsize;
		}

		if (async->start == BTRFS_SUPER_INFO_OFFSET)
			update_super(outbuf);

		ret = pwrite64(outfd, outbuf, size, async->start);
		if (ret < size) {
			if (ret < 0) {
				fprintf(stderr, "Error writing to device %d\n",
					errno);
				err = errno;
			} else {
				fprintf(stderr, "Short write\n");
				err = -EIO;
			}
		}

		pthread_mutex_lock(&mdres->mutex);
		if (err && !mdres->error)
			mdres->error = err;
		mdres->num_items--;
		pthread_mutex_unlock(&mdres->mutex);

		free(async->buffer);
		free(async);
	}
out:
	free(buffer);
	pthread_exit(NULL);
}

static void mdrestore_destroy(struct mdrestore_struct *mdres)
{
	int i;
	pthread_mutex_lock(&mdres->mutex);
	mdres->done = 1;
	pthread_cond_broadcast(&mdres->cond);
	pthread_mutex_unlock(&mdres->mutex);

	for (i = 0; i < mdres->num_threads; i++)
		pthread_join(mdres->threads[i], NULL);

	pthread_cond_destroy(&mdres->cond);
	pthread_mutex_destroy(&mdres->mutex);
	free(mdres->threads);
}

static int mdrestore_init(struct mdrestore_struct *mdres,
			  FILE *in, FILE *out, int num_threads)
{
	int i, ret = 0;

	memset(mdres, 0, sizeof(*mdres));
	pthread_cond_init(&mdres->cond, NULL);
	pthread_mutex_init(&mdres->mutex, NULL);
	INIT_LIST_HEAD(&mdres->list);
	mdres->in = in;
	mdres->out = out;

	if (!num_threads)
		return 0;

	mdres->num_threads = num_threads;
	mdres->threads = calloc(num_threads, sizeof(pthread_t));
	if (!mdres->threads)
		return -ENOMEM;
	for (i = 0; i < num_threads; i++) {
		ret = pthread_create(mdres->threads + i, NULL, restore_worker,
				     mdres);
		if (ret)
			break;
	}
	if (ret)
		mdrestore_destroy(mdres);
	return ret;
}

static int add_cluster(struct meta_cluster *cluster,
		       struct mdrestore_struct *mdres, u64 *next)
{
	struct meta_cluster_item *item;
	struct meta_cluster_header *header = &cluster->header;
	struct async_work *async;
	u64 bytenr;
	u32 i, nritems;
	int ret;

	BUG_ON(mdres->num_items);
	mdres->compress_method = header->compress;

	bytenr = le64_to_cpu(header->bytenr) + BLOCK_SIZE;
	nritems = le32_to_cpu(header->nritems);
	for (i = 0; i < nritems; i++) {
		item = &cluster->items[i];
		async = calloc(1, sizeof(*async));
		if (!async) {
			fprintf(stderr, "Error allocating async\n");
			return -ENOMEM;
		}
		async->start = le64_to_cpu(item->bytenr);
		async->bufsize = le32_to_cpu(item->size);
		async->buffer = malloc(async->bufsize);
		if (!async->buffer) {
			fprintf(stderr, "Error allocing async buffer\n");
			free(async);
			return -ENOMEM;
		}
		ret = fread(async->buffer, async->bufsize, 1, mdres->in);
		if (ret != 1) {
			fprintf(stderr, "Error reading buffer %d\n", errno);
			free(async->buffer);
			free(async);
			return -EIO;
		}
		bytenr += async->bufsize;

		pthread_mutex_lock(&mdres->mutex);
		list_add_tail(&async->list, &mdres->list);
		mdres->num_items++;
		pthread_cond_signal(&mdres->cond);
		pthread_mutex_unlock(&mdres->mutex);
	}
	if (bytenr & BLOCK_MASK) {
		char buffer[BLOCK_MASK];
		size_t size = BLOCK_SIZE - (bytenr & BLOCK_MASK);

		bytenr += size;
		ret = fread(buffer, size, 1, mdres->in);
		if (ret != 1) {
			fprintf(stderr, "Error reading in buffer %d\n", errno);
			return -EIO;
		}
	}
	*next = bytenr;
	return 0;
}

static int wait_for_worker(struct mdrestore_struct *mdres)
{
	int ret = 0;

	pthread_mutex_lock(&mdres->mutex);
	ret = mdres->error;
	while (!ret && mdres->num_items > 0) {
		struct timespec ts = {
			.tv_sec = 0,
			.tv_nsec = 10000000,
		};
		pthread_mutex_unlock(&mdres->mutex);
		nanosleep(&ts, NULL);
		pthread_mutex_lock(&mdres->mutex);
		ret = mdres->error;
	}
	pthread_mutex_unlock(&mdres->mutex);
	return ret;
}

static int restore_metadump(const char *input, FILE *out, int num_threads)
{
	struct meta_cluster *cluster;
	struct meta_cluster_header *header;
	struct mdrestore_struct mdrestore;
	u64 bytenr = 0;
	FILE *in;
	int ret = 0;

	if (!strcmp(input, "-")) {
		in = stdin;
	} else {
		in = fopen(input, "r");
		if (!in) {
			perror("unable to open metadump image");
			return 1;
		}
	}

	cluster = malloc(BLOCK_SIZE);
	if (!cluster) {
		fprintf(stderr, "Error allocating cluster\n");
		if (in != stdin)
			fclose(in);
		return -ENOMEM;
	}

	ret = mdrestore_init(&mdrestore, in, out, num_threads);
	if (ret) {
		fprintf(stderr, "Error initing mdrestore %d\n", ret);
		if (in != stdin)
			fclose(in);
		free(cluster);
		return ret;
	}

	while (1) {
		ret = fread(cluster, BLOCK_SIZE, 1, in);
		if (!ret)
			break;

		header = &cluster->header;
		if (le64_to_cpu(header->magic) != HEADER_MAGIC ||
		    le64_to_cpu(header->bytenr) != bytenr) {
			fprintf(stderr, "bad header in metadump image\n");
			ret = -EIO;
			break;
		}
		ret = add_cluster(cluster, &mdrestore, &bytenr);
		if (ret) {
			fprintf(stderr, "Error adding cluster\n");
			break;
		}

		ret = wait_for_worker(&mdrestore);
		if (ret) {
			fprintf(stderr, "One of the threads errored out %d\n",
				ret);
			break;
		}
	}

	mdrestore_destroy(&mdrestore);
	free(cluster);
	if (in != stdin)
		fclose(in);
	return ret;
}

static void print_usage(void)
{
	fprintf(stderr, "usage: btrfs-image [options] source target\n");
	fprintf(stderr, "\t-r      \trestore metadump image\n");
	fprintf(stderr, "\t-c value\tcompression level (0 ~ 9)\n");
	fprintf(stderr, "\t-t value\tnumber of threads (1 ~ 32)\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	char *source;
	char *target;
	int num_threads = 0;
	int compress_level = 0;
	int create = 1;
	int ret;
	FILE *out;

	while (1) {
		int c = getopt(argc, argv, "rc:t:");
		if (c < 0)
			break;
		switch (c) {
		case 'r':
			create = 0;
			break;
		case 't':
			num_threads = atoi(optarg);
			if (num_threads <= 0 || num_threads > 32)
				print_usage();
			break;
		case 'c':
			compress_level = atoi(optarg);
			if (compress_level < 0 || compress_level > 9)
				print_usage();
			break;
		default:
			print_usage();
		}
	}

	argc = argc - optind;
	if (argc != 2)
		print_usage();
	source = argv[optind];
	target = argv[optind + 1];

	if (create && !strcmp(target, "-")) {
		out = stdout;
	} else {
		out = fopen(target, "w+");
		if (!out) {
			perror("unable to create target file");
			exit(1);
		}
	}

	if (num_threads == 0 && compress_level > 0) {
		num_threads = sysconf(_SC_NPROCESSORS_ONLN);
		if (num_threads <= 0)
			num_threads = 1;
	}

	if (create)
		ret = create_metadump(source, out, num_threads,
				      compress_level);
	else
		ret = restore_metadump(source, out, 1);

	if (out == stdout)
		fflush(out);
	else
		fclose(out);

	return ret;
}
