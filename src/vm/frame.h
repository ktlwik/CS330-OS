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

struct list FT;
struct list swap_list;
#endif /* vm/frame.h */
