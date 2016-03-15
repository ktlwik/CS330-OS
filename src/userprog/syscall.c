#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/init.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

static void syscall_handler (struct intr_frame *);
static void check_args(void *esp, uint32_t argc);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
check_args(void *esp, uint32_t argc)
{
    uint32_t _esp = (uint32_t)esp;
    if((_esp + (argc << 2)) >= 0xc0000000)
        thread_exit();
}

static void 
_halt()
{
    power_off();
}

static void
_exit(void *esp)
{
    struct thread *t = thread_current();
    int32_t ret = *(const int32_t *)(esp + 4);
    t->exit_state = ret;
    thread_exit();
}
/* for sys_create */
static bool
_create(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    unsigned initial_size = *(unsigned *)(esp + 8);
    if(file_name == NULL) return false;
    return filesys_create(file_name, initial_size);
}
/* for sys_remove */
static bool
_remove(void *esp)
{
    const char *file_name = *(const char **)(esp + 4);
    if(file_name == NULL) return false;
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
allocate_fd()
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
    file = filesys_open(file_name);
    if(file == NULL)
    {
        return -1;
    }

    wrapper = (struct fd_wrap *)malloc(sizeof(struct fd_wrap));
    wrapper->file = file;
    wrapper->fd = allocate_fd();
    list_insert_ordered(&t->fd_list, &wrapper->elem, fd_sort, NULL);
    return wrapper->fd;
}

/* for close syscall */
static void
destroy_fd_wrapper(struct fd_wrap *wrap)
{
    file_close(wrap->file);
    list_remove(&wrap->elem);
    free(wrap);
}

static void
_close(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    struct fd_wrap *fd_wrapper;
    fd_wrapper = get_fd_wrapper_by_fd(fd);
    if(fd_wrapper)
        destroy_fd_wrapper(fd_wrapper);
}

/* for sys_file_size */
static int
_file_size(void *esp)
{
    int32_t fd = *(int32_t *)(esp + 4);
    struct fd_wrap *fd_wrapper;
    fd_wrapper = get_fd_wrapper_by_fd(fd);
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
    struct file *file;

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
        return file_tell(fd_wrapper->file);
    }
    return -1;
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t sysnum; 

  sysnum = *(uint32_t *)f->esp;
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
        break;
    case SYS_WAIT:
        check_args(f->esp, 2);
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
    default:
        PANIC("NOT HANDLED");
  }
}
