#include "vm/page.h"
#include <stdbool.h>
#include <hash.h>
#include <debug.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#include "devices/disk.h"
static bool vm_install_stack(struct SPT_elem *);
static bool vm_install_segment(struct SPT_elem *);

void vm_init()
{
  list_init(&FT);
  list_init(&swap_list);
  lock_init(&swap_lock);
  lock_init(&page_lock);
  swap_disk = disk_get(1,1); // swap disk
  free_space = bitmap_create(disk_size(swap_disk) >> 3);
}

unsigned page_hash_func(const struct hash_elem *e, void *aux)
{
    ASSERT(aux == NULL);
    struct SPT_elem *SPT_elem = hash_entry(e, struct SPT_elem, elem);
    return (unsigned)SPT_elem->vaddr;
}

bool page_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
    ASSERT(aux == NULL);
    return hash_entry(a, struct SPT_elem, elem)->vaddr > hash_entry(b, struct SPT_elem, elem)->vaddr;
}


bool palloc_user_page(void *upage, vm_type type, void *aux)
{
    enum intr_level old_level = intr_disable();
    struct thread *t = thread_current();
    bool success = false;
    ASSERT(upage != NULL);
    struct SPT_elem *SPT_elem = (struct SPT_elem *)malloc(sizeof(struct SPT_elem));
    if(SPT_elem != NULL)
    {
      success = true;
      SPT_elem->vaddr = upage;
      SPT_elem->type = type;
      SPT_elem->aux = aux;
      SPT_elem->paddr = NULL;
      SPT_elem->frame_ptr = NULL;

      hash_insert(&t->SPT, &SPT_elem->elem);
    }
    intr_set_level(old_level);
    return success;
}


bool
vm_install_page(struct SPT_elem *SPT_elem, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, SPT_elem->vaddr) == NULL
          && pagedir_set_page (t->pagedir, SPT_elem->vaddr, SPT_elem->paddr, writable));
}

static bool
vm_install_stack(struct SPT_elem *elem)
{
    ASSERT(elem->type == VM_STACK);
    elem->paddr = palloc_get_page(PAL_USER | PAL_ZERO);
    return (elem->paddr == NULL) ? swap_out(elem) : vm_install_page(elem, true);
}

static bool
vm_install_segment(struct SPT_elem *elem)
{
    bool success = false;
    ASSERT(elem->type == VM_SEGMENT);
    elem->paddr = palloc_get_page(PAL_USER);
    if(elem->paddr == NULL)
    {
        return swap_out(elem);
    }

    /* load this page */
    struct file *file = ((struct file **)elem->aux)[0];
    size_t page_read_bytes = ((size_t *)elem->aux)[1];
    bool writable = (bool)((int32_t *)elem->aux)[2];
    off_t ofs = (off_t)((off_t *)elem->aux)[3];
    file_seek(file, ofs);
    if(file_read(file, elem->paddr, page_read_bytes) != (int) page_read_bytes)
    {
        return false;
    }
    memset(elem->paddr + page_read_bytes, 0, PGSIZE - page_read_bytes);

    success = vm_install_page(elem, writable);
    return success;
}

bool
vm_install(struct SPT_elem *elem)
{
  bool success = false;
  ASSERT(elem->paddr == NULL);
  if(!elem->frame_ptr)
  {
    // no frame allocated.
    if(elem->type == VM_STACK)
    {
        success = vm_install_stack(elem);
    }
    else if(elem->type == VM_SEGMENT)
    {
        success = vm_install_segment(elem);
    }

    // double insertion due to recursion.
    if(elem->frame_ptr == NULL)
    {
      struct FRAME_elem *FRAME_elem = (struct FRAME_elem *)malloc(sizeof(struct FRAME_elem));

      FRAME_elem->SPT_ptr = elem;
      FRAME_elem->holder = thread_current();
      FRAME_elem->swaped = MEMORY;

      elem->frame_ptr = FRAME_elem;
      ASSERT(elem->paddr != NULL);
      list_push_back(&FT, &FRAME_elem->elem);
    }
  }
  else
  {
    // needed swap_in
    success = swap_in(elem);
    if(success)
    {
        if(elem->frame_ptr->swaped != MEMORY)
        {
          elem->frame_ptr->swaped = MEMORY;
          ASSERT(elem->paddr != NULL);
          list_push_back(&FT, &(elem->frame_ptr->elem));
        }
    }
  }
  if(!success) printf("install fail\n");
  ASSERT(pagedir_get_page(elem->frame_ptr->holder->pagedir, elem->vaddr));
  //printf("install %x to %x of %d\n", elem->vaddr, elem->paddr, elem->frame_ptr->holder->tid);
  return success;
}
