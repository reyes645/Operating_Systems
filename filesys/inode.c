#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
/* Number of sector indexes in an inode*/
#define NUM_INDEXES 12
#define DIRECT_SECTORS 10
#define POINTERS_IN_SECTOR 128
#define SECTORS_BEFORE_SL (DIRECT_SECTORS + POINTERS_IN_SECTOR)
#define INDEX_OF_FL 10
#define INDEX_OF_SL 11

static char zeros[BLOCK_SECTOR_SIZE];

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t data_blocks[NUM_INDEXES];  /* Pointers to sectors */
    off_t length;                             /* File size in bytes. */
    unsigned magic;                           /* Magic number. */
    block_sector_t parent_directory;          /* Sector of parent's inode*/
    uint32_t is_directory;                    /* 1 if directory, 0 if file*/
    uint32_t unused[112];                     /* Not used. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
    struct lock inode_lock;
    struct lock dir_lock;
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos, off_t length) 
{
  ASSERT (inode != NULL);
  const struct inode_disk *data = &inode->data;
  // only return the sector if the position is less than the
  // current length of the file
  if (pos <= length) 
  {
    // find out which sector this is
    block_sector_t sector_index = pos / BLOCK_SECTOR_SIZE;
    
    // if sector is one of the 10 direct blocks, return
    // index of that position in the direct blocks
    if (sector_index < DIRECT_SECTORS) 
    {
      return data->data_blocks[sector_index];
      
    } else if (sector_index < SECTORS_BEFORE_SL) 
    {
      // if sector is between 10 and 138, it fits in the first level
      // indirect block
      block_sector_t fl_index = data->data_blocks[INDEX_OF_FL];
      void *fl_block = calloc (1, BLOCK_SECTOR_SIZE);
      if (fl_block == NULL)
      {
        return -1;
      }
      // read sector where the first level index is stored
      block_read (fs_device, fl_index, fl_block);
      // subtract 10 direct blocks to get index in first level block
      sector_index -= DIRECT_SECTORS;
      block_sector_t final_index = ((block_sector_t *) fl_block)[sector_index];
      free (fl_block);
      return final_index;
    } else
    {
      // if index is greater than 138, it cannot fit in a single indirect block
      // must look in second level indirect blocks
      // subtract 138 to
      sector_index -= SECTORS_BEFORE_SL;
      void *sl_block = calloc (1, BLOCK_SECTOR_SIZE);
      void *fl_block = calloc (1, BLOCK_SECTOR_SIZE);
      if (sl_block == NULL || fl_block == NULL)
      {
        return -1;
      }
      // read sector at index of second level indirect block, data_blocks[11]
      block_sector_t inode_index = data->data_blocks[INDEX_OF_SL];
      block_read (fs_device, inode_index, sl_block);
      // divide by number of pointers in sector to get index in the second level
      // sector
      int fl_index = sector_index / POINTERS_IN_SECTOR;
      // read sector at that index in the second level block
      block_sector_t sl_index = ((block_sector_t *) sl_block)[fl_index];
      block_read (fs_device, sl_index, fl_block);
      // remainder by 128 to get the index in the first level block
      sector_index %= POINTERS_IN_SECTOR;
      block_sector_t final_index = ((block_sector_t *) fl_block)[sector_index];

      free (sl_block);
      free (fl_block);
      return final_index;
    }
  } else 
  {
    // position is past end of file, cannot find sector
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

// Allocate space on disk for a first level indirect block at sector SECTOR
// Updates NUM_SECTORS and STARTING_SECTOR with new indices after calculating
// how much space is taken up. Writes in all zeros initially for the data
// and the indices of where the data blocks are located
static bool
allocate_first_level (block_sector_t *sector, size_t *num_sectors,
                      size_t *starting_sector)
{
  size_t length = *num_sectors;
  size_t start = *starting_sector;
  // find the number of first level indices needed
  int num_fl = (length >= POINTERS_IN_SECTOR) ? POINTERS_IN_SECTOR : length;

  if (start < POINTERS_IN_SECTOR)
  {
    block_sector_t *fl_block = calloc (1, BLOCK_SECTOR_SIZE);
    if (fl_block == NULL)
    {
      return false;
    }
    
    // if start is 0, this sector has not been allocated yet,
    // allocate a new sector
    if(start == 0)
    {
      free_map_allocate (1, sector);

    }else
    {
      // otherwise this sector already exists, read in exisiting information
      // into fl_block
      block_read (fs_device, *sector, fl_block);
    }
    
    // beginnning at start, allocate new sectors and write them to disk
    // initialized to zeros
    int index;
    for (index = start; index < num_fl; index++) 
    {
      free_map_allocate (1, &fl_block[index]);
      block_write (fs_device, fl_block[index], zeros);
    }
    
    // write updated fl_block back into disk
    block_write (fs_device, *sector, fl_block);
    free (fl_block);
  }
  
  // update the sector information for use in second level indirect
  // block if we need it
  *num_sectors -= num_fl;
  *starting_sector = (start < POINTERS_IN_SECTOR) ? 0 :
                      start - POINTERS_IN_SECTOR;
  return true;
}

// ensure that we have enough space in the file system to allocate
// num_sectors
static bool
check_length (size_t num_sectors)
{
  size_t num_free = free_map_count ();
  size_t total_sectors = num_sectors;
  if(num_sectors > DIRECT_SECTORS)
  {
    total_sectors++;
  }
  if (num_sectors > SECTORS_BEFORE_SL) 
  {
    total_sectors++;
    size_t num_fl = DIV_ROUND_UP ((num_sectors - SECTORS_BEFORE_SL),
                                    POINTERS_IN_SECTOR);
    total_sectors += num_fl;
  }

  return total_sectors <= num_free;
}

static bool extend (struct inode_disk *disk_inode, size_t total_sectors, 
                    size_t starting_sector);

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length,
              block_sector_t parent_directory, int is_directory)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      // get number of sectors needed
      size_t sectors = bytes_to_sectors (length);

      // check if we have enough space
      if(!check_length (sectors))
      {
        free (disk_inode);
        return false;
      }
      
      // update metadata
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->parent_directory = parent_directory;
      disk_inode->is_directory = is_directory;
      // extend the file from offset 0
      if (!extend (disk_inode, sectors, 0))
      {
        return false;
      }

      // write the inode's sector itself to disk
      block_write (fs_device, sector, disk_inode);
      success = true;
      free (disk_inode);
    }
    
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
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
  lock_init (&inode->inode_lock);
  block_read (fs_device, inode->sector, &inode->data);
  if (inode->data.is_directory)
  {
    lock_init (&inode->dir_lock);
  }
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
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

// return INODE's parent sector number
block_sector_t 
inode_get_parent (const struct inode *inode)
{
  return inode->data.parent_directory;
}

// return if INODE is a directory
bool
inode_is_dir (const struct inode *inode)
{
  return inode->data.is_directory;
}

// return INODE's open count
int
inode_open_cnt (const struct inode *inode)
{
  return inode->open_cnt;
}

// return INODE's directory lock
struct lock *
get_dir_lock (struct inode *inode)
{
  return &inode->dir_lock;
}

/* Used in inode_close to free all of the data stored using INODE*/
static bool 
release_data (struct inode *inode)
{
  struct inode_disk *data = &inode->data;
  int sectors = bytes_to_sectors (data->length);

  //Calculate number of blocks stored directly
  int num_direct = sectors <= DIRECT_SECTORS ? sectors : DIRECT_SECTORS;
  int index;
  for (index = 0; index < num_direct; index++)
  {
    free_map_release (data->data_blocks[index], 1);
  }

  //Update number of blocks left to release
  sectors -= num_direct;

  if (sectors > 0)
  {
    //Read in first level indirect block
    block_sector_t *fl_block = calloc (1, BLOCK_SECTOR_SIZE);
    if (fl_block == NULL)
    {
      return false;
    }
    block_read (fs_device, data->data_blocks[INDEX_OF_FL], (void *) fl_block);

    //Calculate number of blocks stored using first level block
    int num_blocks = (sectors <= POINTERS_IN_SECTOR) ? sectors :
                      POINTERS_IN_SECTOR;
    for (index = 0; index < num_blocks; index++)
    {
      free_map_release (fl_block[index], 1);
    }

    //Release first level block and update blocks left
    free_map_release (data->data_blocks[INDEX_OF_FL], 1);
    sectors -= POINTERS_IN_SECTOR;
    free (fl_block);
  }
  
  if (sectors > 0)
  {
    //Read in second level indirect block
    block_sector_t *sl_block = calloc (1, BLOCK_SECTOR_SIZE);
    block_sector_t *fl_block = calloc (1, BLOCK_SECTOR_SIZE);
    if (fl_block == NULL || sl_block == NULL)
    {
      return false;
    }
    block_read (fs_device, data->data_blocks[INDEX_OF_SL], (void *) sl_block);

    //Calculate number of first level blocks stored in second level
    int num_sl = DIV_ROUND_UP (sectors, POINTERS_IN_SECTOR);
    int sl_index;
    for (sl_index = 0; sl_index < num_sl; sl_index++)
    {
      //Read in first level block and release all its data
      block_read (fs_device, sl_block[sl_index], fl_block);
      int num_fl = (sectors <= POINTERS_IN_SECTOR) ? sectors :
                    POINTERS_IN_SECTOR;

      int fl_index;
      for(fl_index = 0; fl_index < num_fl; fl_index++){
        free_map_release (fl_block[fl_index], 1);
      }

      //Release first level block and update sectors left
      free_map_release (sl_block[sl_index], 1);
      sectors -= num_fl;
    }

    //Release second level block
    free_map_release (data->data_blocks[INDEX_OF_SL], 1);
    free (sl_block);
    free (fl_block);
  }
  return true;
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
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
          if (!release_data (inode))
          {
            return;
          }
          free_map_release (inode->sector, 1);
        }

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
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = 
              byte_to_sector (inode, offset, inode_length (inode));
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   File growth implemented */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  off_t current_length = inode->data.length;
  off_t new_size = offset + size;
  off_t file_size = (current_length < new_size) ? new_size : current_length;
  size_t final_sectors = bytes_to_sectors (new_size);
  size_t current_sectors = bytes_to_sectors (current_length);

  if(final_sectors > current_sectors)
  {
    if(!check_length (final_sectors - current_sectors))
    {
      return 0;
    }
    
    //Extends the file by storing the indexes of the new data block
    //but doesn't update length yet to prevent other proccesses from
    //accessesing where we are still writing
    if (!extend (&inode->data, final_sectors, current_sectors))
    {
      return 0;
    }
  }

  if(current_length < new_size)
  {
    //Make sure two processes cannot extend the same file at once
    lock_acquire (&inode->inode_lock);
  }
  
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset, file_size);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = file_size - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  if(current_length < new_size)
  {
    //Updates the new size if the file has been extended
    inode->data.length = new_size;
    block_write (fs_device, inode->sector, &inode->data);
    lock_release (&inode->inode_lock);
  }

  free (bounce);
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

/* Extends a file starting from STARTING_SECTOR until the total file length is
   TOTAL_SECTORS. All new data sectors are filled with zeroes and their sector 
   number is stored in the inode either directly or through an indirect block
   also stored in a sector.

   Can be used to extend an already existing file or to create a new file, in
   which case STARTING_SECTOR is 0*/
static bool
extend (struct inode_disk *disk_inode, size_t total_sectors,
        size_t starting_sector)
{
  //Calculate how many blocks will be stored directly
  int num_direct = (total_sectors >= DIRECT_SECTORS) ? DIRECT_SECTORS :
                    total_sectors;
  int index;
  for (index = starting_sector; index < num_direct; index++)
  {
    free_map_allocate (1, &disk_inode->data_blocks[index]);
    block_write (fs_device, disk_inode->data_blocks[index], zeros);
  }

  //Update how many sectors left until we reach starting_sector
  starting_sector = (starting_sector < DIRECT_SECTORS) ? 0 :
                      starting_sector - DIRECT_SECTORS;
  //How many more sectors do we need to store
  total_sectors -= num_direct;

  if(total_sectors > 0)
  {
    //Store next 128 blocks in first level indirect block
    if (!allocate_first_level (&disk_inode->data_blocks[INDEX_OF_FL], 
                                &total_sectors, &starting_sector))
    {
      return false;
    }
    
    if(total_sectors > 0)
    {
      block_sector_t *sl_block = calloc(1, BLOCK_SECTOR_SIZE);
      if (sl_block == NULL)
      {
        return false;
      }

      if(starting_sector == 0)
      {
        free_map_allocate (1, &disk_inode->data_blocks[INDEX_OF_SL]);
      }else
      {
        block_read (fs_device, disk_inode->data_blocks[INDEX_OF_SL], sl_block);
      }
      
      // Calculate number of first level blocks needed and which one to
      // starting at
      int num_sl = DIV_ROUND_UP (total_sectors, POINTERS_IN_SECTOR);

      int sl_index;
      for (sl_index = 0; sl_index < num_sl; sl_index++)
      {
        //For each entry in the second level block, fill up a first level block
        //and store it's sector number in the second level block
        if(!allocate_first_level (&sl_block[sl_index], &total_sectors,
                                  &starting_sector))
        {
          return false;
        }
      }

      //Write the second level indirect block to memory
      block_write (fs_device, disk_inode->data_blocks[INDEX_OF_SL], sl_block);
      free (sl_block);
    }
  }
  return true;
}
