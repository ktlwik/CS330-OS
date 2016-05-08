#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"
#ifdef EFILESYS
#include "threads/thread.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#endif
/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
#ifdef EFILESYS
  CWD = dir_open_root();
#endif
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
#ifdef CFILESYS
    disk_cache_WB_all();
#endif
#ifdef EFILESYS
    dir_close(CWD);
#endif
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  disk_sector_t inode_sector = 0;
#ifdef EFILESYS
  struct dir *dir = NULL;
  char *real_name = NULL;
  struct dir *start;

  const char *start_of_path;
  if(thread_current()->CWD == NULL)
  {
      thread_current()->CWD = dir_open_root();
  }
  if(name[0] == '/')
  {
      start = dir_open_root();
      start_of_path = name + 1;
  }
  else
  {
      start = dir_reopen(thread_current()->CWD);
      start_of_path = name;
  }
  char *path_dup = malloc(strlen(start_of_path) + 1);
  strlcpy(path_dup, start_of_path, strlen(start_of_path) + 1);
  resolve_dir_path(path_dup, start, &real_name, &dir);
#else
  struct dir *dir = dir_open_root ();
  const char *real_name = name;
#endif
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && 
#ifdef EFILESYS 
                  inode_create (inode_sector, initial_size, INODE_FILE)
#else
                  inode_create(inode_sector, initial_size)
#endif
                  && dir_add (dir, real_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
#ifdef EFILESYS
  free(path_dup);
#endif
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct inode *inode = NULL;
#ifdef EFILESYS
  struct dir *dir = NULL;
  char *real_name = NULL;
  struct dir *start;

  const char *start_of_path;
  if(thread_current()->CWD == NULL)
  {
      thread_current()->CWD = dir_open_root();
  }
  if(name[0] == '/')
  {
      start = dir_open_root();
      start_of_path = name + 1;
  }
  else
  {
      start = dir_reopen(thread_current()->CWD);
      start_of_path = name;
  }
  char *path_dup = malloc(strlen(start_of_path) + 1);
  strlcpy(path_dup, start_of_path, strlen(start_of_path) + 1);
  resolve_dir_path(path_dup, start, &real_name, &dir);
#else
  struct dir *dir = dir_open_root ();
  const char *real_name = name;
#endif
  if (dir != NULL)
    dir_lookup (dir, real_name, &inode);
  dir_close (dir);
#ifdef EFILESYS
  free(path_dup);
#endif
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
#ifdef EFILESYS
  struct dir *dir = NULL;
  char *real_name = NULL;
  struct dir *start;

  const char *start_of_path;
  if(thread_current()->CWD == NULL)
  {
      thread_current()->CWD = dir_open_root();
  }
  if(name[0] == '/')
  {
      start = dir_open_root();
      start_of_path = name + 1;
  }
  else
  {
      start = dir_reopen(thread_current()->CWD);
      start_of_path = name;
  }
  char *path_dup = malloc(strlen(start_of_path) + 1);
  strlcpy(path_dup, start_of_path, strlen(start_of_path) + 1);
  resolve_dir_path(path_dup, start, &real_name, &dir);
#else
  struct dir *dir = dir_open_root ();
  const char *real_name = name;
#endif
  bool success = true;
#ifdef EFILESYS
  struct inode *inode = NULL;
  if(dir_lookup(dir, real_name, &inode))
  {
      if(is_inode_dir(inode))
      {
          struct dir *try_rm;
          disk_sector_t rm_target_sec = inode_get_inumber(inode);
          inode_close(inode);
          inode = inode_reopen(dir_get_inode(thread_current()->CWD));
          while(inode && inode_get_inumber(inode) != ROOT_DIR_SECTOR)
          {
              if(rm_target_sec == inode_get_inumber(inode))
              {
                  success = false;
                  inode_close(inode);
                  inode = NULL;
                  break;
              }
              try_rm = dir_open(inode);
              dir_lookup(try_rm, "..", &inode);
              dir_close(try_rm);
          }
      }
      else
          inode_close(inode);
  }
#endif
  success = success && dir != NULL && dir_remove (dir, real_name);
  dir_close (dir);
#ifdef EFILESYS
  free(path_dup);
#endif

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
#ifdef EFILESYS
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
#else
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
#endif
  free_map_close ();
  printf ("done.\n");
}
