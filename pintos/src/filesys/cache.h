#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "off_t.h"
#include "devices/block.h"

/* Initialize cache */
void cache_init(void);

/* flush cache entries into disk */
void cache_flush(struct block *fs_device);

/* read chunk_size of data from cache starting from sector_index at offset into destination*/
void cache_read(struct block *fs_device, block_sector_t sector_index, void *dst, off_t offset, int chunk_size);

/* write chunk_size of data into cache starting from sector_index at offset from source */
void cache_write(struct block *fs_device, block_sector_t sector_index, void *src, off_t offset, int chunk_size);

/* invalidate all entries of cache */
void cache_invalidate(struct block *fs_device);

/* get the cache statistics */
int cache_get_stats(long long *accsee_count, long long *hit_count, long long *miss_count);

#endif
