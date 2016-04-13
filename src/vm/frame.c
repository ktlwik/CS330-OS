#include <bitmap.h>
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"

void frame_destroy(struct SPT_elem *elem)
{
  if(elem->frame_ptr && elem->frame_ptr->swaped == DISK)
    {
        bitmap_set_multiple(free_space, elem->frame_ptr->start, 1, false);
    }

    if(elem->paddr)
    {
      write_back(elem);

      pagedir_clear_page(thread_current()->pagedir, elem->vaddr);
      palloc_free_page(elem->paddr);
      elem->paddr = NULL;
    };

    if(elem->type == VM_MMAP)
    {
        file_close(((struct file **)elem->aux)[0]);
    }
    if(elem->aux) free(elem->aux);

    hash_delete(&(thread_current()->SPT), &elem->elem); 
    if(elem->frame_ptr)
    {
        list_remove(&(elem->frame_ptr->elem));
        free(elem->frame_ptr);
    }
    free(elem);
}

void write_back(struct SPT_elem *elem)
{
    if(elem->type == VM_MMAP)
    {
        ASSERT(elem->aux != NULL);
        struct file *file = ((struct file **)elem->aux)[0];
        size_t page_read_bytes = ((size_t *)elem->aux)[1];
        off_t ofs = (off_t)((off_t *)elem->aux)[2];
        if(elem->paddr && pagedir_is_dirty((elem->frame_ptr->holder)->pagedir, elem->vaddr))
        {
            file_write_at(file, elem->paddr, page_read_bytes, ofs);
        }
    }
}


