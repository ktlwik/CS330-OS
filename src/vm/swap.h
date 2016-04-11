#ifndef SWAP_H
#define SWAP_H
#include "vm/page.h"
#include "devices/disk.h"
#include "threads/synch.h"
#include <bitmap.h>

struct SPT_elem;

typedef enum {NA, DISK, MEMORY} swap_state;

struct disk *swap_disk;
struct bitmap *free_space;
struct lock swap_lock;

bool swap_in(struct SPT_elem *);
bool swap_out(struct SPT_elem *);
#endif /* vm/swap.h */
