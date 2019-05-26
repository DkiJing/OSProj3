#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/bolck.h"

void init_buffer(void);
void close_buffer(void);

void buffer_read(block_sector_t sector, void *target);
void buffer_write(block_sector_t sector, const void *source);

#endif
