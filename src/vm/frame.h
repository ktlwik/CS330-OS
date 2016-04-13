#ifndef FRAME_H
#define FRAME_H
#include <hash.h>
#include "vm/page.h"
#include "filesys/off_t.h"
#include "devices/disk.h"

struct FRAME_elem
{
    struct SPT_elem *SPT_ptr;
    struct thread *holder;
    swap_state swaped;
    struct list_elem elem;

    disk_sector_t start;
};

typedef uint32_t Mapid_t;
struct mmap_wrap
{
    void *addr;
    Mapid_t mapid;
    struct list_elem elem;
    struct list SPTE_list;
};

struct list FT;
struct list swap_list;

void frame_destroy(struct SPT_elem *);
void write_back(struct SPT_elem *);
void hash_write_back_all(struct hash_elem *);
#endif /* vm/frame.h */
