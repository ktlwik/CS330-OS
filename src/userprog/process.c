#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"
#ifdef VM
#include "vm/page.h"
#include "vm/frame.h"
#include <hash.h>
#endif
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *unused;
  tid_t tid;
  void *arr[3];
  bool load_fail = false;
  struct semaphore start_sema;
 
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  memset(fn_copy, 0, PGSIZE);
  strlcpy (fn_copy, file_name, PGSIZE);

  fn_copy = strtok_r(fn_copy, " ", &unused);
  sema_init(&start_sema, 0);

  /* Create a new thread to execute FILE_NAME. */
  arr[0] = fn_copy;
  arr[1] = &start_sema;
  arr[2] = &load_fail;
  lock_acquire(&process_execute_lock);
  tid = thread_create (fn_copy, PRI_DEFAULT, start_process, arr);
  lock_release(&process_execute_lock);
  sema_down(&start_sema);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  if(load_fail == true) tid = TID_ERROR;
  return tid;
}
/* A thread function that loads a user process and makes it start
   running. */
static void
start_process (void *args)
{
  char *file_name = ((char **)args)[0];
  struct intr_frame if_;
  struct thread *t = thread_current();
  struct semaphore *start_sema = ((struct semaphore **)args)[1];
  bool *load_fail = ((bool **)args)[2];
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);
  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success)
  {
      *load_fail = true;
      list_remove(&t->child_elem);
      sema_up(start_sema);
      t->load_fail = true;
      thread_exit ();
      NOT_REACHED();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
#ifdef EFILESYS
  if(thread_current()->CWD == NULL)
  {
      thread_current()->CWD = dir_open_root();
  }
#endif
  sema_up(start_sema);
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *t = get_thread_by_tid(child_tid);
  int exit_state = 0;
  if(t == NULL)
  {
      return -1;
  }
  ASSERT(t->tid == child_tid);
  if(t->dying_fin == true)
      return -1;
  sema_down(&t->wait_sema);
  exit_state = t->exit_state;
  t->dying_fin = true;
  list_remove(&t->child_elem);
  sema_up(&t->fin_sema);
  return exit_state;
}
#ifdef VM
static void
destroy_alloc(struct hash_elem *e)
{
    struct SPT_elem *elem;
    elem = hash_entry(e, struct SPT_elem, elem);
    frame_destroy(elem);
}
#endif
/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *curr = thread_current ();
  uint32_t *pd;
  struct fd_wrap *wrapper;

  if(!lock_held_by_current_thread(&filesys_lock))
      lock_acquire(&filesys_lock);

  while(!list_empty(&curr->fd_list))
  {
      wrapper = list_entry(list_pop_front(&curr->fd_list), struct fd_wrap, elem);
      file_close(wrapper->file);
      free(wrapper);
  }
#ifdef VM
  struct mmap_wrap *mwrapper;
  while(!list_empty(&curr->mmap_list))
  {
      mwrapper = list_entry(list_pop_front(&curr->mmap_list), struct mmap_wrap, elem);
      free(mwrapper);
  }
  ASSERT(list_size(&curr->mmap_list) == 0)
#endif
  ASSERT(list_size(&curr->fd_list) == 0);
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */

  if(curr->executable != NULL)
  {
      file_close(curr->executable);
  }
  lock_release(&filesys_lock);

#ifdef VM
  if(!lock_held_by_current_thread(&page_lock))
    lock_acquire(&page_lock);
  struct hash_iterator iter;
  while(!hash_empty(&curr->SPT))
  {
      hash_first(&iter, &curr->SPT);
      if(hash_next(&iter))
        destroy_alloc(hash_cur(&iter));
  }
  lock_release(&page_lock);
#endif

  if(curr->load_fail == false)
  {
      sema_up(&curr->wait_sema);
      printf("%s: exit(%d)\n", curr->name, curr->exit_state);
      sema_down(&curr->fin_sema);
  }
 
  pd = curr->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      curr->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
static bool
setup_args(void **esp, const char *file_name)
{
    char *name_dup = (char *)file_name;
    enum {DQUOTE, QUOTE, UNQUOTE} state = UNQUOTE;
    char *ptr, *endptr;
    int len;
    *(name_dup + strlen(name_dup)) = ' ';
    len = strlen(name_dup);

    /* last byte of string */
    ptr = name_dup + len + 1;
    **(uint32_t **)esp = 0;
    while(ptr -= 1, ptr != name_dup)
    {
        if(state == UNQUOTE)
        {
            if (*ptr == ' ')
            {
                if(*(ptr + 1) == 0) continue;
                *ptr = 0;
            }
            else if(*ptr == '\'')
            {
                state = QUOTE;
                continue;
            }
            else if(*ptr == '"')
            {
                state = DQUOTE;
                continue;
            }
        }
        else if(state == QUOTE && *ptr == '\'')
        {
            state = UNQUOTE;
            continue;

        }
        else if(state == DQUOTE && *ptr =='"')
        {
            state = UNQUOTE;
            continue;
        }
        *esp = *esp - 1;
        **(uint8_t **)esp = *ptr;
    }

    *esp = *esp - 1;
    **(uint8_t **)esp = *ptr;
    endptr = *esp;
    ptr = PHYS_BASE - 1;
    ASSERT(state == UNQUOTE);

    /* word align */
    while(((uint32_t)(*esp) & 3) != 0)
    {
        (*esp) --;
    }

    /* last argv ptr */
    len = 0; /* stand for argc */
    *(uint32_t **)esp -= 1;
    **(uint32_t **)esp = 0;

    /* pointer for argv[] */
    while(ptr != endptr)
    {
        if(*(ptr - 1) == 0)
        {
            *(uint32_t **)esp -= 1;
            **(uint32_t **)esp = (uint32_t)ptr;
            len += 1;
        }
        ptr -= 1;
    }
    *(uint32_t **)esp -= 1;
    **(uint32_t **)esp = (uint32_t)ptr;
    len += 1;

    *(uint32_t **)esp -= 3;
    *(*(uint32_t **)esp + 2) = (uint32_t)(*(uint32_t **)esp + 3);           /* argv */
    *(*(uint32_t **)(esp) + 1) = len;                                       /* argc */
    *(*(uint32_t **)(esp) + 0) = 0;                                         /* retaddr */
    //hex_dump(0, *esp, (int) ((size_t) PHYS_BASE - (size_t) *esp), true); /* test */

    return true;

}
/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();
  /* Open executable file. */
  lock_acquire(&filesys_lock);
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;
  setup_args(esp, file_name);
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if(file != NULL)
  {
    t->executable = file;
    file_deny_write(file);
  }
  lock_release(&filesys_lock);
  return success;
}

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);
/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Do calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      /* Get a page of memory. */
#ifndef VM
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL){
        return false;}

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }
#else
      void **args = malloc(sizeof(void *) * 4);
      if(args == NULL)
          PANIC("malloc fail\n");
      args[0] = (void *)file;
      args[1] = (void *)page_read_bytes;
      args[2] = (void *)writable;
      args[3] = (void *)ofs;
      ofs += page_read_bytes;
      if(!palloc_user_page(upage, VM_SEGMENT, args)) return false;
#endif
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
#ifndef VM
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (!success)
        palloc_free_page (kpage);
    }
#else
  bool success = palloc_user_page(((uint8_t *)PHYS_BASE) - PGSIZE, VM_STACK, NULL);
#endif
  if(success)
    *esp = PHYS_BASE;

  return success;
}
/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
#ifndef VM
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
#endif
