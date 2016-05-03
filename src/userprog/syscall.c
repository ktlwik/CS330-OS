#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "lib/string.h"
#include "filesys/directory.h"
#include "filesys/inode.h"
#include "userprog/process.h"
#include "devices/input.h"
#ifdef VM
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#endif
typedef int pid_t;

static void syscall_handler (struct intr_frame *);
static void check_args(void *esp, uint32_t argc);

void
syscall_init (void) 
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
check_args(void *esp, uint32_t argc)
{
    uint32_t _esp = (uint32_t)esp;
    if((_esp + (argc << 2)) >= 0xc0000000)
        thread_exit();
}
/* for sys_halt */
static void 
_halt(void)
{
    power_off();
}

/* for sys_exit */
static void
_exit(void *esp)
{
    struct thread *t = thread_current();
    int32_t ret = *(const int32_t *)(esp + 4);
    t->exit_state = ret;
    thread_exit();
}

/* for sys_exec */
static pid_t
_exec(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    char fn_copy[0x100];
    char *name;
    char *unused;
    bool check_exists = false;
    strlcpy(fn_copy, file_name, 0x100);
    name = strtok_r(fn_copy, " ", &unused);

    lock_acquire(&filesys_lock);
    struct dir *dir = dir_open_root ();
    struct inode *inode = NULL;

    check_exists = (dir != NULL && dir_lookup (dir, fn_copy, &inode));
    dir_close(dir);
    inode_close (inode);
    lock_release(&filesys_lock);
    return check_exists == true ? process_execute(file_name) : -1;
}

/* for sys_wait */
static int
_wait(void *esp)
{
    pid_t pid = *(pid_t *)(esp + 4);

    return process_wait(pid);
}

/* for sys_create */
static bool
_create(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    unsigned initial_size = *(unsigned *)(esp + 8);
    if(file_name == NULL) thread_exit();
    lock_acquire(&filesys_lock);
    return filesys_create(file_name, initial_size);
}
/* for sys_remove */
static bool
_remove(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    if(file_name == NULL) return false;
    lock_acquire(&filesys_lock);
    return filesys_remove(file_name);
}

/* fd_utils */
static bool
fd_sort(const struct list_elem *A, const struct list_elem *B, void *aux)
{
    const struct fd_wrap *fdA = list_entry(A, struct fd_wrap, elem);
    const struct fd_wrap *fdB = list_entry(B, struct fd_wrap, elem);
    ASSERT(aux == NULL);

    return(fdA->fd < fdB->fd)? true:false;
}

static struct fd_wrap *
get_fd_wrapper_by_fd(int32_t fd)
{
    struct list *fd_list = &thread_current()->fd_list;
    struct list_elem *e;
    struct fd_wrap *wrapper;
    for(e = list_begin(fd_list); e != list_end(fd_list); e = list_next(e))
    {
        wrapper = list_entry(e, struct fd_wrap, elem);
        if(wrapper->fd == fd)
            return wrapper;
    }
    return NULL;
}

/* for sys_open */
static int
allocate_fd(void)
{
    struct list *fd_list = &thread_current()->fd_list;
    struct list_elem *e;
    struct fd_wrap *wrapper;
    int32_t __fd = 2;
    for(e = list_begin(fd_list); e != list_end(fd_list); e = list_next(e))
    {
        wrapper = list_entry(e, struct fd_wrap, elem);
        if(wrapper->fd != __fd)
            break;
        __fd++;
    }
    return __fd;
}

static int
_open(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    struct file *file = NULL;
    struct fd_wrap *wrapper = NULL;
    struct thread *t = thread_current();
    if(file_name == NULL)
    {
        thread_exit();
    }
    lock_acquire(&filesys_lock);
    file = filesys_open(file_name);
    if(file == NULL)
    {
        return -1;
    }

    wrapper = (struct fd_wrap *)malloc(sizeof(struct fd_wrap));
    if(wrapper == NULL)
    {
        file_close(file);
        return -1;
    }
    wrapper->file = file;
    wrapper->fd = allocate_fd();
    list_insert_ordered(&t->fd_list, &wrapper->elem, fd_sort, NULL);
    return wrapper->fd;
}

static void
_close(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    struct fd_wrap *fd_wrapper;
    lock_acquire(&filesys_lock);
    fd_wrapper = get_fd_wrapper_by_fd(fd);
    if(fd_wrapper)
    {
        file_close(fd_wrapper->file);
        list_remove(&fd_wrapper->elem);
        free(fd_wrapper);
    }
}

/* for sys_file_size */
static int
_file_size(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    struct fd_wrap *fd_wrapper;
    fd_wrapper = get_fd_wrapper_by_fd(fd);
    lock_acquire(&filesys_lock);
    if(fd_wrapper != NULL)
        return file_length(fd_wrapper->file);
    return -1;
}

/* for sys_read */
static int
_read(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    void *buffer = *(void **)(esp + 8);
    uint32_t len = *(uint32_t *)(esp + 12);
    uint32_t ret = 0;
    struct fd_wrap *fd_wrapper;

    if(fd == 0)
    {
        for(ret = 0; ret < len; ret++)
        {
            *(char *)(buffer + ret) = input_getc();
        }
            
    }
    else if(fd == 1)
    {
        thread_exit();
    }
    else
    {
        fd_wrapper = get_fd_wrapper_by_fd(fd);
        if(fd_wrapper != NULL)
        {
            lock_acquire(&filesys_lock);
            ret = file_read(fd_wrapper->file, buffer, len);
        }
    }
    return ret;
}

/* for sys_write */
static int
_write(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    void *buffer = *(void **)(esp + 8);
    uint32_t len = *(uint32_t *)(esp + 12);
    uint32_t ret = 0;
    struct fd_wrap *fd_wrapper;
    if(fd == 0)
    {
        thread_exit();
    }
    else if(fd ==1)
    {
        putbuf(buffer, len);
        ret = len;
    }
    else
    {
        fd_wrapper = get_fd_wrapper_by_fd(fd);
        if(fd_wrapper != NULL)
        {
            lock_acquire(&filesys_lock);
            ret = file_write(fd_wrapper->file, buffer, len);
        }
    }
    return ret;
 
}

/* for sys_seek */
static void
_seek(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    unsigned position = *(unsigned *)(esp + 8);
    struct fd_wrap *fd_wrapper;
    fd_wrapper = get_fd_wrapper_by_fd(fd);
    if(fd_wrapper != NULL)
    {
        lock_acquire(&filesys_lock);
        file_seek(fd_wrapper->file, position);
    }
}

/* for sys_tell */
static unsigned
_tell(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    struct fd_wrap *fd_wrapper;
    fd_wrapper = get_fd_wrapper_by_fd(fd);
    if(fd_wrapper != NULL)
    {
        lock_acquire(&filesys_lock);
        return file_tell(fd_wrapper->file);
    }
    return -1;
}
#ifdef VM

/* fd_utils */
static bool
mapid_sort(const struct list_elem *A, const struct list_elem *B, void *aux)
{
    const struct mmap_wrap *mmapA = list_entry(A, struct mmap_wrap, elem);
    const struct mmap_wrap *mmapB = list_entry(B, struct mmap_wrap, elem);
    ASSERT(aux == NULL);

    return(mmapA->mapid < mmapB->mapid)? true:false;
}

static struct mmap_wrap *
get_map_wrapper_by_mapid(Mapid_t mapid)
{
    struct list *mmap_list = &thread_current()->mmap_list;
    struct list_elem *e;
    struct mmap_wrap *wrapper;
    for(e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
    {
        wrapper = list_entry(e, struct mmap_wrap, elem);
        if(wrapper->mapid == mapid)
            return wrapper;
    }
    return NULL;
}

/* for sys_open */
static Mapid_t
allocate_mapid(void)
{
    struct list *mmap_list = &thread_current()->mmap_list;
    struct list_elem *e;
    struct mmap_wrap *wrapper;
    uint32_t __mapid = 1;
    for(e = list_begin(mmap_list); e != list_end(mmap_list); e = list_next(e))
    {
        wrapper = list_entry(e, struct mmap_wrap, elem);
        if(wrapper->mapid != __mapid)
            break;
        __mapid++;
    }
    return __mapid;
}


static Mapid_t 
_mmap(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    void *addr = *(void **)(esp + 8);
    struct thread *t = thread_current();
    struct fd_wrap *fd_wrapper = NULL;
    struct mmap_wrap *mmap_wrapper = NULL;
    struct SPT_elem *elem;
    lock_acquire(&page_lock);
    Mapid_t mapid = 1;
    if(fd == 0 || fd == 1 || ((uint32_t)addr & 0xfff) || addr == NULL || pagedir_get_page(thread_current()->pagedir, addr) != NULL)
        mapid = -1;
    else
    {
      fd_wrapper = get_fd_wrapper_by_fd(fd);
      if(fd_wrapper == NULL)
      {
          mapid = -1;
      }
      struct hash_iterator iter;
      hash_first(&iter, &t->SPT);
      while(hash_next(&iter))
      {
          elem = hash_entry(hash_cur(&iter), struct SPT_elem, elem);
          if(elem->vaddr == addr)
          {
              mapid = -1;
              break;
          }
      }

      uint32_t read_bytes = file_length(fd_wrapper->file);
      if(read_bytes == 0)
        mapid = -1;

      if(mapid != (uint32_t)-1)
      {
          mmap_wrapper = (struct mmap_wrap *)malloc(sizeof(struct mmap_wrap));
          if(mmap_wrapper == NULL)
          {
              mapid = -1;
          }
          else
          {
            mmap_wrapper->mapid = allocate_mapid();
            mmap_wrapper->addr = addr;
            list_init(&mmap_wrapper->SPTE_list);
            struct SPT_elem *alloc;

            void *_aux[2];
            void **args;
            off_t ofs = 0;
            while(read_bytes > 0)
            {
              alloc = NULL;
              _aux[0] = &alloc;
              args = malloc(sizeof(void *) * 3);
              _aux[1] = args;
              args[0] = file_reopen(fd_wrapper->file);
              args[1] = (void *)((read_bytes > PGSIZE) ? PGSIZE : read_bytes);
              args[2] = (void *)ofs;
              palloc_user_page(addr, VM_MMAP, _aux);
              list_push_back(&mmap_wrapper->SPTE_list, &alloc->mmap_elem);
              if(read_bytes < PGSIZE) break;
              ofs += PGSIZE;
              read_bytes -= PGSIZE;
              addr += PGSIZE;
            }
            mapid = mmap_wrapper->mapid;
            list_insert_ordered(&t->mmap_list, &mmap_wrapper->elem, mapid_sort, NULL);
          }
      }
    }
    lock_release(&page_lock);
    return mapid;
}

static void
_unmap(void *esp)
{
    Mapid_t mapping = *(Mapid_t *)(esp + 4);
    struct mmap_wrap *mmap_wrapper = get_map_wrapper_by_mapid(mapping);
    struct SPT_elem *elem;

    if(mmap_wrapper)
    {
        while(!list_empty(&mmap_wrapper->SPTE_list))
        {
            elem = list_entry(list_pop_front(&mmap_wrapper->SPTE_list), struct SPT_elem, mmap_elem);
            ASSERT(elem->frame_ptr->holder == thread_current());
            frame_destroy(elem);
        }
        list_remove(&mmap_wrapper->elem);
        free(mmap_wrapper);
    }
}
#endif
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t sysnum;

  sysnum = *(uint32_t *)f->esp;
  thread_current()->syscall_esp = f->esp;
  thread_current()->is_syscall = true;
  switch(sysnum)
  {
    case SYS_HALT:
        check_args(f->esp, 1);
        _halt();
        break;
    case SYS_EXIT:
        check_args(f->esp, 2);
        _exit(f->esp);
        break;
    case SYS_EXEC:
        check_args(f->esp, 2);
        f->eax = _exec(f->esp);
        break;
    case SYS_WAIT:
        check_args(f->esp, 2);
        f->eax = _wait(f->esp);
        break;
    case SYS_CREATE:
        check_args(f->esp, 3);
        f->eax = _create(f->esp);
        break;
    case SYS_REMOVE:
        check_args(f->esp, 2);
        f->eax = _remove(f->esp);
        break;
    case SYS_OPEN:
        check_args(f->esp, 2);
        f->eax = _open(f->esp);
        break;
    case SYS_FILESIZE:
        check_args(f->esp, 2);
        f->eax = _file_size(f->esp);
        break;
    case SYS_READ:
        check_args(f->esp, 4);
        f->eax = _read(f->esp);
        break;
    case SYS_WRITE:
        check_args(f->esp, 4);
        f->eax = _write(f->esp);
        break;
    case SYS_SEEK:
        check_args(f->esp, 3);
        _seek(f->esp);
        break;
    case SYS_TELL:
        check_args(f->esp, 2);
        f->eax = _tell(f->esp);
        break;
    case SYS_CLOSE:
        check_args(f->esp, 2);
        _close(f->esp);
        break;
#ifdef VM
    case SYS_MMAP:
        check_args(f->esp, 3);
        f->eax = _mmap(f->esp);
        break;
    case SYS_MUNMAP:
        check_args(f->esp, 2);
        _unmap(f->esp);
        break;
#endif
#ifdef EFILESYS
    case SYS_CHDIR:
    case SYS_MKDIR:
    case SYS_READDIR:
    case SYS_ISDIR:
    case SYS_INUMBER:
#endif
    default:
        PANIC("NOT HANDLED");
  }
  if(lock_held_by_current_thread(&filesys_lock))
      lock_release(&filesys_lock);
  thread_current()->is_syscall = false;
  //printf("SYSCALL BY %s : ret: %x\n", thread_current()->name, f->eax);
}
