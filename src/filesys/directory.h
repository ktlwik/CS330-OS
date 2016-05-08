#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/disk.h"

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

struct inode;

/* Opening and closing directories. */
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);
struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);

/* Reading and writing. */
bool dir_lookup (const struct dir *, const char *name, struct inode **);
bool dir_add (struct dir *, const char *name, disk_sector_t);
bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);

#ifdef EFILESYS
bool dir_create (disk_sector_t, size_t, disk_sector_t);
bool resolve_dir_path(char *, struct dir *, char **, struct dir **);
bool dir_make(char *, struct dir *);
bool dir_change(char *, struct dir *);
bool is_inode_dir(const struct inode *);
#else
bool dir_create (disk_sector_t, size_t);
#endif
#endif /* filesys/directory.h */
