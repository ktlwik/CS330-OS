#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
#ifdef EFILESYS
#define DISK_ENTRY_NUM ((size_t)(DISK_SECTOR_SIZE / sizeof(disk_sector_t)))
#define DIRECT_NUM ((size_t)123)
#define NOT_EXIST_SEC ((disk_sector_t) -1)
struct entry_block
{
    disk_sector_t no[DISK_ENTRY_NUM];
};

struct entry_block_ptrs
{
    struct entry_block index;
    struct entry_block *iblocks[DISK_ENTRY_NUM];
};
#endif

struct inode_disk
  {
#ifdef EFILESYS
    uint32_t type;
#else
    disk_sector_t start;                /* First data sector. */
#endif
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
#ifdef EFILESYS
    disk_sector_t disk_sec[DIRECT_NUM];
    disk_sector_t Iblocks_sec;
    disk_sector_t DIblocks_sec;
#else
    uint32_t unused[125];
#endif
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode cache. */
#ifdef CFILESYS
struct lock cache_lock;
struct disk_cache
{
    struct list_elem elem;
    bool is_dirty;
    struct disk *disk;
    disk_sector_t no;
    char buffer[DISK_SECTOR_SIZE];
};

static void disk_read_with_cache(struct disk *, disk_sector_t, void *, off_t, size_t);
static void disk_write_with_cache(struct disk *, disk_sector_t, void *, off_t, size_t);

struct list disk_cache_list;
#endif

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct entry_block *iblock_ptr;
    struct entry_block_ptrs *diblock_ptr;
  };

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
#ifdef EFILESYS
bool
is_inode_dir(const struct inode *inode)
{
    return inode->data.type == INODE_DIR;
}
static void
load_iblock(struct inode *inode)
{
    if(inode->iblock_ptr != NULL) return;
    ASSERT(inode->data.Iblocks_sec != (disk_sector_t)-1);
    struct entry_block *iblock = malloc(sizeof(struct entry_block));
    disk_read_with_cache(filesys_disk, inode->data.Iblocks_sec, iblock, 0, DISK_SECTOR_SIZE);
    inode->iblock_ptr = iblock;
}

static void
load_diblock(struct inode *inode, size_t di_no)
{
    if(inode->diblock_ptr == NULL)
    {
        inode->diblock_ptr = malloc(sizeof(struct entry_block_ptrs));
        disk_read_with_cache(filesys_disk, inode->data.DIblocks_sec, &(inode->diblock_ptr->index), 0, DISK_SECTOR_SIZE);
        memset(&inode->diblock_ptr->iblocks, 0, DISK_SECTOR_SIZE);
    }
    ASSERT(inode->diblock_ptr->index.no[di_no] != NOT_EXIST_SEC);
    inode->diblock_ptr->iblocks[di_no] = malloc(DISK_SECTOR_SIZE);
    disk_read_with_cache(filesys_disk, inode->diblock_ptr->index.no[di_no], inode->diblock_ptr->iblocks[di_no], 0, DISK_SECTOR_SIZE);
}
#endif
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
#ifndef EFILESYS
  if (pos < inode->data.length)
    return inode->data.start + pos / DISK_SECTOR_SIZE;
  else
    return NOT_EXIST_SEC;
#else
  off_t rtn = -1;
  if(pos < inode->data.length)
  {
      disk_sector_t idx = pos / DISK_SECTOR_SIZE;
      if(idx < DIRECT_NUM)
          rtn = inode->data.disk_sec[idx];
      else if (idx >= DIRECT_NUM && idx < DIRECT_NUM + DISK_ENTRY_NUM)
      {
          load_iblock((struct inode *)inode);
          rtn = inode->iblock_ptr->no[idx - DIRECT_NUM];
      }
      else if(idx >= DIRECT_NUM + DISK_ENTRY_NUM && idx < DIRECT_NUM + DISK_ENTRY_NUM * DISK_ENTRY_NUM)
      {
          size_t current_sector = idx - DIRECT_NUM - DISK_ENTRY_NUM;
          size_t di_no = current_sector / DISK_ENTRY_NUM;
          size_t i_no = current_sector % DISK_ENTRY_NUM;

          load_diblock((struct inode *)inode, di_no);
          ASSERT(inode->diblock_ptr->iblocks[di_no] != NULL);
          rtn = inode->diblock_ptr->iblocks[di_no]->no[i_no];
      }
      else
      {
          PANIC("FILE SIZE IS TOO BIG\n");
      }
//  printf("translate: %d to %d\n", idx, rtn);
  }
  return rtn;
#endif
}

#ifdef EFILESYS
static bool
extend_inode(struct inode_disk *disk_inode, struct entry_block *iblocks, struct entry_block_ptrs *diblocks, disk_sector_t sectors)
{
    size_t i = bytes_to_sectors(disk_inode->length);
    static char zeros[DISK_SECTOR_SIZE];
    disk_sector_t *target;
    //printf("EXTEND INODE %d to %d\n", i, sectors);
    for(; i < sectors; i++)
    {
      //  printf("install %d\n", i);
        // direct block
        if(i < DIRECT_NUM)
        {
            target = &disk_inode->disk_sec[i];
        }
        // indirect block
        else if(i >= DIRECT_NUM && i < DIRECT_NUM + DISK_ENTRY_NUM)
        {
            ASSERT(iblocks != NULL); 
            ASSERT(disk_inode->Iblocks_sec != NOT_EXIST_SEC);
            target = &iblocks->no[i - DIRECT_NUM];
        }
        // doubly indirect block
        else if(i >= DIRECT_NUM + DISK_ENTRY_NUM && i < DIRECT_NUM + DISK_ENTRY_NUM + DISK_ENTRY_NUM * DISK_ENTRY_NUM)
        {
            ASSERT(diblocks != NULL);
            ASSERT(disk_inode->DIblocks_sec != NOT_EXIST_SEC);
            size_t current_sector = i - DIRECT_NUM - DISK_ENTRY_NUM;
            size_t di_no = current_sector / DISK_ENTRY_NUM;
            size_t i_no = current_sector % DISK_ENTRY_NUM;
            ASSERT(diblocks->iblocks[di_no] != NULL);
            target = &(diblocks->iblocks[di_no]->no[i_no]);
        }
        else
        {
            PANIC("FILE IS TOO BIG");
        }

        if(free_map_allocate(1, target))
        {
            //printf("memset %d sector\n", *target);
            disk_write_with_cache(filesys_disk, *target, zeros, 0, DISK_SECTOR_SIZE);
        }
        else
            return false;
    }
//    printf("etend fin\n");
    return true;
}
#endif
/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
#ifdef CFILESYS
  list_init(&disk_cache_list);
  lock_init(&cache_lock);
#endif
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
#ifdef EFILESYS
bool
inode_create (disk_sector_t sector, off_t length, uint32_t type)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);
  ASSERT(type == INODE_FILE || type == INODE_DIR);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->type = type;

      disk_inode->Iblocks_sec = -1;
      disk_inode->DIblocks_sec = -1;
      struct entry_block *iblocks = NULL;
      struct entry_block_ptrs *diblocks = NULL;
      if(sectors >= DIRECT_NUM)
      {
          if(free_map_allocate(1, &disk_inode->Iblocks_sec))
          {
              iblocks = malloc(sizeof(struct entry_block));
          }
          else
          {
              return false;
          }
      }

      if(sectors >= DIRECT_NUM + DISK_ENTRY_NUM)
      {
          if(free_map_allocate(1, &disk_inode->DIblocks_sec))
          {
              diblocks = malloc(sizeof(struct entry_block_ptrs));
              memset(&diblocks->index, -1, sizeof(struct entry_block));
              memset(&diblocks->iblocks, 0, DISK_SECTOR_SIZE);
              size_t max_indirect_nums = (sectors - DIRECT_NUM - DISK_ENTRY_NUM) / DISK_ENTRY_NUM;
              size_t i;
              for(i = 0; i <= max_indirect_nums; i++)
              {
                  if(free_map_allocate(1, &(diblocks->index.no[i])))
                  {
                      diblocks->iblocks[i] = malloc(DISK_SECTOR_SIZE);
                      memset(diblocks->iblocks[i], -1, DISK_SECTOR_SIZE);
                      disk_write_with_cache(filesys_disk, diblocks->index.no[i], diblocks->iblocks[i], 0, DISK_SECTOR_SIZE);
                  }

              }
          }
      }

      success = extend_inode(disk_inode, iblocks, diblocks, sectors);
      disk_inode->length = length;
      if(diblocks)
      {
          size_t i;
          for(i = 0; ; i++)
          {
              if(diblocks->index.no[i] != NOT_EXIST_SEC)
              {
                  ASSERT(diblocks->iblocks[i] != NULL);
                  disk_write_with_cache(filesys_disk, diblocks->index.no[i], diblocks->iblocks[i], 0, DISK_SECTOR_SIZE);
                  free(diblocks->iblocks[i]);
              }
              else
              {
                  break;
              }
          }
          disk_write_with_cache(filesys_disk, disk_inode->DIblocks_sec, diblocks, 0, DISK_SECTOR_SIZE);
      }
      if(iblocks)
      {
          disk_write_with_cache(filesys_disk, disk_inode->Iblocks_sec, iblocks, 0, DISK_SECTOR_SIZE);
          free(iblocks);
      }
      disk_write_with_cache(filesys_disk, sector, disk_inode, 0, DISK_SECTOR_SIZE);
      free (disk_inode);
    }
  return success;
}
#else
bool
inode_create (disk_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate (sectors, &disk_inode->start))
        {
          disk_write (filesys_disk, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[DISK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                disk_write (filesys_disk, disk_inode->start + i, zeros); 
            }
          success = true; 
        } 
      free (disk_inode);
    }
  return success;
}

#endif


/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes); e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

#ifdef EFILESYS
  inode->iblock_ptr = NULL;
  inode->diblock_ptr = NULL;
#endif

#ifndef CFILESYS
  disk_read (filesys_disk, inode->sector, &inode->data);
#else
  disk_read_with_cache (filesys_disk, inode->sector, &inode->data, 0, DISK_SECTOR_SIZE);
#endif
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
       /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
#ifdef EFILESYS
          off_t i;
          for(i = 0; i < inode->data.length; i += DISK_SECTOR_SIZE)
          {
              free_map_release(byte_to_sector(inode, i), 1);
          }
#else
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
#endif
        }
#ifdef EFILESYS
      if(inode->iblock_ptr) free(inode->iblock_ptr);
      if(inode->diblock_ptr)
      {
          disk_sector_t i;
          for(i = 0; i < DISK_ENTRY_NUM; i++)
          {
              if(inode->diblock_ptr->iblocks[i])
                  free(inode->diblock_ptr->iblocks[i]);
          }
          free(inode->diblock_ptr);
      }
#endif
      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
#ifndef CFILESYS
  uint8_t *bounce = NULL;
#endif
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
#ifndef CFILESYS
      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) 
        {
          /* Read full sector directly into caller's buffer. */
          disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          disk_read (filesys_disk, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
#else
      disk_read_with_cache(filesys_disk, sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
#endif
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
#ifndef CFILESYS
  free (bounce);
#endif

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
#ifndef CFILESYS
  uint8_t *bounce = NULL;
#endif
  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      off_t inode_left = inode_length (inode) - offset;
#ifdef EFILESYS
      if(inode_left < size)
      {
          disk_sector_t need_sectors = bytes_to_sectors(offset + size);
          struct inode_disk  *disk_inode = &inode->data;
          struct entry_block *iblocks = NULL;
          struct entry_block_ptrs *diblocks = NULL;
          if(need_sectors >= DIRECT_NUM)
          {
              if(disk_inode->Iblocks_sec == NOT_EXIST_SEC)
              {
                  if(free_map_allocate(1, &disk_inode->Iblocks_sec))
                  {
                      iblocks = malloc(sizeof(struct entry_block));
                  }
                  else
                  {
                      return false;
                  }
              }
              else
              {
                  load_iblock(inode);
                  iblocks = inode->iblock_ptr;
              }
          }

          if(need_sectors >= DIRECT_NUM + DISK_ENTRY_NUM)
          {
              if(disk_inode->DIblocks_sec == NOT_EXIST_SEC && free_map_allocate(1, &disk_inode->DIblocks_sec))
              {
                  diblocks = malloc(sizeof(struct entry_block_ptrs));
                  memset(&diblocks->index, -1, sizeof(struct entry_block));
                  memset(&diblocks->iblocks, 0, DISK_SECTOR_SIZE);
                  inode->diblock_ptr = diblocks;
              }
              else
              {
                diblocks = inode->diblock_ptr;
              }
              size_t max_indirect_nums = (need_sectors - DIRECT_NUM - DISK_ENTRY_NUM) / DISK_ENTRY_NUM;
              size_t i;
              for(i = 0; i <= max_indirect_nums; i++)
              {
                  if(diblocks->index.no[i] != NOT_EXIST_SEC)
                  {
                      load_diblock(inode, i);
                  }
                  else if(free_map_allocate(1, &(diblocks->index.no[i])))
                  {
                      diblocks->iblocks[i] = malloc(DISK_SECTOR_SIZE);
                      memset(diblocks->iblocks[i], -1, DISK_SECTOR_SIZE);
                      disk_write_with_cache(filesys_disk, diblocks->index.no[i], diblocks->iblocks[i], 0, DISK_SECTOR_SIZE);
                  }
              }
          }

          if(!extend_inode(disk_inode, iblocks, diblocks, need_sectors))
          {
              break;
          }
          else
          {
              disk_inode->length = offset + size;
              inode_left = inode_length(inode) - offset;
              if(diblocks)
              {
                  size_t i;
                  for(i = 0; ; i++)
                  {
                      if(diblocks->index.no[i] != NOT_EXIST_SEC)
                      {
                          ASSERT(diblocks->iblocks[i] != NULL);
                          disk_write_with_cache(filesys_disk, diblocks->index.no[i], diblocks->iblocks[i], 0, DISK_SECTOR_SIZE);
                      }
                      else
                      {
                          break;
                      }
                  }
                  disk_write_with_cache(filesys_disk, disk_inode->DIblocks_sec, diblocks, 0, DISK_SECTOR_SIZE);
              }
              if(iblocks)
              {
                  disk_write_with_cache(filesys_disk, disk_inode->Iblocks_sec, iblocks, 0, DISK_SECTOR_SIZE);
              }
              disk_write_with_cache(filesys_disk, inode->sector, disk_inode, 0, DISK_SECTOR_SIZE);
          }
      }
#endif
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
#ifndef CFILESYS
      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) 
        {
          /* Write full sector directly to disk. */
          disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            disk_read (filesys_disk, sector_idx, bounce);
          else
            memset (bounce, 0, DISK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          disk_write (filesys_disk, sector_idx, bounce); 
        }
#else
      disk_write_with_cache(filesys_disk, sector_idx, (void *)(buffer + bytes_written), sector_ofs, chunk_size);
#endif
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
#ifndef CFILESYS
  free (bounce);
#endif
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

#ifdef CFILESYS


static void
disk_cache_WB(struct disk_cache *cache)
{
    if(cache->is_dirty == true)
    {
        disk_write(cache->disk, cache->no, cache->buffer);
    }
}

void 
disk_cache_WB_all()
{
    struct disk_cache *disk;
    while(!list_empty(&disk_cache_list))
    {
        disk = list_entry(list_pop_front(&disk_cache_list), struct disk_cache, elem);
        disk_cache_WB(disk);
        free(disk);
    }
}

static struct disk_cache *
lookup_disk_cache(struct disk * disk, disk_sector_t no)
{
    struct list_elem *elem;
    struct disk_cache *cache = NULL;
    // cache HIT
    for(elem = list_begin(&disk_cache_list); elem != list_end(&disk_cache_list); elem = list_next(elem))
    {
        cache = list_entry(elem, struct disk_cache, elem);
        if(cache->disk == disk && cache->no == no)
        {
            return cache;
        }
    }

    // EVICT
    if(list_size(&disk_cache_list) >= 64)
    {
        disk_cache_WB(list_entry(list_pop_back(&disk_cache_list), struct disk_cache, elem));
        free(cache);
    }

    // LOAD TO DISK
    cache = malloc(sizeof(struct disk_cache));
    if(cache != NULL)
    {
        cache->disk = disk;
        cache->no = no;
        cache->is_dirty = false;
        disk_read(cache->disk, cache->no, cache->buffer);
        list_push_front(&disk_cache_list, &cache->elem);
    }
    return cache;
}

static void
disk_read_with_cache(struct disk *disk, disk_sector_t no, void *buffer, off_t start, size_t size)
{
    ASSERT(size <= DISK_SECTOR_SIZE);
    ASSERT(start >= 0 && start <= DISK_SECTOR_SIZE);
    lock_acquire(&cache_lock);
    struct disk_cache *cache = lookup_disk_cache(disk, no);
    memcpy(buffer, cache->buffer + start, size);
    lock_release(&cache_lock);
}

static void
disk_write_with_cache(struct disk *disk, disk_sector_t no, void *buffer, off_t start, size_t size)
{
    ASSERT(start + size <= DISK_SECTOR_SIZE);
    ASSERT(start >= 0 && start <= DISK_SECTOR_SIZE);
    lock_acquire(&cache_lock);
    struct disk_cache *cache = lookup_disk_cache(disk, no);
    memcpy(cache->buffer + start, buffer, size);
    cache->is_dirty = true;
    lock_release(&cache_lock);
}
#endif
