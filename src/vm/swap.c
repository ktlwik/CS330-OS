#include <stdbool.h>
#include "vm/swap.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "devices/disk.h"
#include "userprog/pagedir.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/interrupt.h"
#include <debug.h>

// get page from swap disk
bool
swap_in(struct SPT_elem *elem)
{
  struct list_elem *e;
  struct FRAME_elem *felem;
  uint8_t *new_page;
  int i;
  bool writable;
  for(e = list_begin(&swap_list); e != list_end(&swap_list); e = list_next(e))
  {
      felem = list_entry(e, struct FRAME_elem, elem);
      if(felem == elem->frame_ptr)
      {
          ASSERT(felem->swaped == DISK);
          new_page = palloc_get_page(PAL_USER);
          if(new_page == NULL)
          {
              return swap_out(elem);
          }
          elem->paddr = new_page;
          for(i = 0; i < 8; i++)
              disk_read(swap_disk, (felem->start << 3) + i, elem->paddr + DISK_SECTOR_SIZE * i);
          writable = (elem->type == VM_SEGMENT) ? (bool)((int32_t *)elem->aux)[2] : true;
          bitmap_set_multiple(free_space, felem->start, 1, false);
          list_remove(&felem->elem);
          return vm_install_page(elem, writable);
      }
  }
  return false;
}

// swap page to disk
bool
swap_out(struct SPT_elem *elem)
{
  ASSERT(!list_empty(&FT));
  
  struct list_elem *e = list_pop_front(&FT);
  struct FRAME_elem *felem = list_entry(e, struct FRAME_elem, elem);

  size_t swap_idx;
  int i;

  //printf("swap out: %d's %x %x\n", felem->holder->tid, felem->SPT_ptr->vaddr, felem->SPT_ptr->paddr);
  ASSERT(felem->swaped == MEMORY);
  // if mmaped, rewrite to it
  write_back(felem->SPT_ptr);
  pagedir_clear_page(felem->holder->pagedir, felem->SPT_ptr->vaddr);
  
  swap_idx = bitmap_scan_and_flip(free_space, 0, 1, false);
  if(swap_idx == BITMAP_ERROR) PANIC("KERNEL PANIC DUE TO FULL SWAP DISK");

  felem->swaped = DISK;
  felem->start = swap_idx;
  void *paddr = felem->SPT_ptr->paddr;
  for(i = 0; i < 8; i++)
      disk_write(swap_disk, (felem->start << 3) + i, paddr + DISK_SECTOR_SIZE * i);
  
  // free page
  felem->SPT_ptr->paddr = NULL;
  palloc_free_page(paddr);
  list_push_front(&swap_list, &felem->elem);
  return vm_install(elem);
}
