#include <debug.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#define CACHE_NUM_ENTRIES 64
#define CACHE_NUM_CHANCES 1

struct cache_block
{
  struct lock cache_block_lock;
  block_sector_t disk_sector_index;
  uint8_t data[BLOCK_SECTOR_SIZE];
  bool valid;
  bool dirty;
  size_t chances_remaining;
};

/* cache entries */
static struct cache_block cache[CACHE_NUM_ENTRIES];

/* lock for updating cache entries */
static struct lock cache_update_lock;

/* prevent flush on uninitialized cache */
static bool cache_initialized = false;

/* cache statistics */
static long long cache_hit_count;
static long long cache_miss_count;
static long long cache_access_count;

static struct lock cache_hit_count_lock;
static struct lock cache_miss_count_lock;
static struct lock cache_access_count_lock;

void
cache_init(void)
{
  /* initlize locks of cache */
  lock_init(&cache_update_lock);
  lock_init(&cache_hit_count_lock);
  lock_init(&cache_miss_count_lock);
  lock_init(&cache_access_count_lock);

  cache_hit_count = 0;
  cache_miss_count = 0;
  cache_access_count = 0;

  /* initilize each cache block */
  int i;
  for(i = 0; i < CACHE_NUM_ENTRIES; i++){
    cache[i].valid = false;
    lock_init(&cache[i].cache_block_lock);
  }
  cache_initialized = true;
}

static void
cache_flush_block_index(struct block *fs_device, int index)
{
  ASSERT(lock_held_by_current_thread(&cache[index].cache_block_lock));
  ASSERT(cache[index].valid == true && cache[index].dirty == true);

  block_write(fs_device, cache[index].disk_sector_index, cache[index].data);
  cache[index].dirty = false;
}

void
cache_flush(struct block *fs_device)
{
  ASSERT(fs_device != NULL);
  
  if(!cache_initialized) return;
  lock_acquire(&cache_update_lock);
  
  /* write each cache block to disk */
  int i;
  for(i = 0; i < CACHE_NUM_ENTRIES; i++){
    lock_acquire(&cache[i].cache_block_lock);
    if(cache[i].valid && cache[i].dirty)
      cache_flush_block_index(fs_device, i);
    lock_release(&cache[i].cache_block_lock);
  }
}

void
cache_invalidate(struct block *fs_device)
{
  if(!cache_initialized) return;
  lock_acquire(&cache_update_lock);
  
  /* invalidate each cache entry */
  int i;
  for(i = 0; i < CACHE_NUM_ENTRIES; i++){
    lock_acquire(&cache[i].cache_block_lock);
    if(cache[i].valid && cache[i].dirty)
      cache_flush_block_index(fs_device, i);
    cache[i].valid = false;
    lock_release(&cache[i].cache_block_lock);
  } 
  lock_release(&cache_update_lock);
}

static void
cache_increment_hit_count(void)
{
  lock_acquire(&cache_hit_count_lock);
  cache_hit_count++;
  lock_release(&cache_hit_count_lock);
}

static void
cache_increment_miss_count(void)
{
  lock_acquire(&cache_miss_count_lock);
  cache_miss_count++;
  lock_release(&cache_miss_count_lock);
}

static void
cache_increment_access_count(void)
{
  lock_acquire(&cache_access_count_lock);
  cache_access_count++;
  lock_release(&cache_access_count_lock);
}

int
cache_get_stats(long long *access_count, long long *hit_count, long long *miss_count)
{
  if(access_count == NULL || hit_count == NULL || miss_count == NULL || !cache_initialized)
    return -1;
  
  lock_acquire(&cache_hit_count_lock);
  lock_acquire(&cache_miss_count_lock);
  lock_acquire(&cache_access_count_lock);

  *access_count = cache_access_count;
  *hit_count = cache_hit_count;
  *miss_count = cache_miss_count;

  lock_release(&cache_access_count_lock);
  lock_release(&cache_miss_count_lock);
  lock_release(&cache_hit_count_lock);
  
  return 0;
}

/* find a cache entry to evict and return its index */
static int
cache_evict(struct block *fs_device, block_sector_t sector_index)
{
  static size_t clock_position = 0;
  lock_acquire(&cache_update_lock);

  /* check sector_index is not already in cache */
  int i;
  for(i = 0; i < CACHE_NUM_ENTRIES; i++){
    lock_acquire(&cache[i].cache_block_lock);
    if(cache[i].valid && (cache[i].disk_sector_index == sector_index)){
      lock_release(&cache_update_lock);
      return (-i) - 1;
    } 
    lock_release(&cache[i].cache_block_lock);
  }
  
  /* clock algorithm */
  while(true){
    i = clock_position;
    clock_position++;
    clock_position %= CACHE_NUM_ENTRIES;
    
    lock_acquire(&cache[i].cache_block_lock);
    if(!cache[i].valid){
      lock_release(&cache_update_lock);
      return i;
    }
    if(cache[i].chances_remaining == 0) break;
    cache[i].chances_remaining--;
    lock_release(&cache[i].cache_block_lock);
  }
  lock_release(&cache_update_lock);
  if(cache[i].dirty)
    cache_flush_block_index(fs_device, i);
  cache[i].valid = false;
  return i;
}

static void
cache_replace(struct block *fs_device, int index, block_sector_t sector_index, bool is_whole_block_write)
{
  ASSERT(lock_held_by_current_thread(&cache[index].cache_block_lock));
  ASSERT(cache[index].valid == false);
  
  if(!is_whole_block_write)
    block_read(fs_device, sector_index, cache[index].data);
  cache[index].valid = true;
  cache[index].dirty = false;
  cache[index].disk_sector_index = sector_index;
  cache[index].chances_remaining = CACHE_NUM_CHANCES;  
}

static int
cache_get_block_index(struct block *fs_device, block_sector_t sector_index, bool is_whole_block_write)
{
  cache_increment_access_count();
  /* check sector_index is in cache */
  int i;
  for(i = 0; i < CACHE_NUM_ENTRIES; i++){
    lock_acquire(&cache[i].cache_block_lock);
    if(cache[i].valid && (cache[i].disk_sector_index == sector_index)){
      /* hit */
      cache_increment_hit_count();
      break;
    }
    lock_release(&cache[i].cache_block_lock);
  }
  /* evict if sector_index is not in cache */
  if(i == CACHE_NUM_ENTRIES){
    i = cache_evict(fs_device, sector_index);
    if(i >= 0){
      cache_increment_miss_count();
      cache_replace(fs_device, i, sector_index, is_whole_block_write);
    }
    else{
      cache_increment_hit_count();
      i = -(i + 1);
    }
  } 
  ASSERT(lock_held_by_current_thread(&cache[i].cache_block_lock));
  return i;
}

void
cache_read(struct block *fs_device, block_sector_t sector_index, void *dst, off_t offset, int chunk_size)
{
  ASSERT(fs_device != NULL);
  ASSERT(cache_initialized == true);
  ASSERT(offset >= 0 && chunk_size >= 0);
  ASSERT((offset + chunk_size) <= BLOCK_SECTOR_SIZE);
  int i = cache_get_block_index(fs_device, sector_index, false);
  ASSERT(lock_held_by_current_thread(&cache[i].cache_block_lock));
  ASSERT(cache[i].valid == true);
  
  memcpy(dst, cache[i].data + offset, chunk_size);
  cache[i].chances_remaining = CACHE_NUM_CHANCES;
  lock_release(&cache[i].cache_block_lock);
}

void
cache_write(struct block *fs_device, block_sector_t sector_index, void *src, off_t offset, int chunk_size)
{
  ASSERT(fs_device != NULL);
  ASSERT(cache_initialized == true);
  ASSERT(offset >= 0 && chunk_size >= 0);
  ASSERT((offset + chunk_size) <= BLOCK_SECTOR_SIZE);
  int i;
  if(chunk_size == BLOCK_SECTOR_SIZE)
    i = cache_get_block_index(fs_device, sector_index, true);
  else
    i = cache_get_block_index(fs_device, sector_index, false);
  ASSERT(lock_held_by_current_thread(&cache[i].cache_block_lock));
  ASSERT(cache[i].valid == true);
  
  memcpy(cache[i].data + offset, src, chunk_size);
  cache[i].dirty = true;
  cache[i].chances_remaining = CACHE_NUM_CHANCES;
  lock_release(&cache[i].cache_block_lock);
}
