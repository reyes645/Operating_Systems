#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt, block_sector_t parent)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry),
                        parent, 1);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens the parent directory of DIR and returns a dir * to is
   Returns NULL if inode_get_parent fails*/
struct dir 
*dir_open_parent (struct dir *dir) 
{
  block_sector_t parent_sector = inode_get_parent (dir->inode);
  if (!parent_sector)
  {
    return NULL;
  }

  return dir_open (inode_open (parent_sector));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. 
   While adding an entry, no other operations can be done to the
   directory being added to.*/
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct lock *dir_lock = get_dir_lock (dir_get_inode (dir));
  lock_acquire (dir_lock);

  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  lock_release (dir_lock);
  return success;
}

/* Checks if the directory storing INODE is ok to remove.
   A directory is able to be removed if it is not the root,
   is currently empty, and is not opened in any process.
   Returns true if all of these conditions are met.*/
static bool
dir_can_remove (struct inode *inode)
{
  struct dir *dir = dir_open (inode);
  struct dir *root = dir_open_root ();

  block_sector_t root_inumber = inode_get_inumber (dir_get_inode (root));
  block_sector_t inumber = inode_get_inumber (inode);
  char name[NAME_MAX + 1];

  dir_close(root);
  bool success = !(dir_readdir (dir, name) || inumber == root_inumber)
                  && !(inode_open_cnt (inode) > 1);
  free (dir);
  return success;
}

/*Searches DIR for a file whose inode is stored at SECTOR.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP.*/
static bool
lookup_sector (struct dir *dir, block_sector_t sector,
              struct dir_entry *ep, off_t *ofsp)
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && e.inode_sector == sector) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. 
   While deleting an entry, no other operation can be performed
   on the directory being changed*/
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir *current_dir = dir;
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  bool found = false;
  /* Find directory entry. */
  if (!strcmp (name, CURRENT_DIRECTORY)) 
  {
    //Search for DIR in parent directory
    block_sector_t sector = inode_get_inumber (dir_get_inode (dir));
    current_dir = dir_open_parent (dir);
    found = lookup_sector (current_dir, sector, &e, &ofs);

  } else {
    found = lookup (current_dir, name, &e, &ofs);
  }
  
  if (!found) {
    goto done;
  }
  
  struct lock *dir_lock = get_dir_lock (dir_get_inode (current_dir));
  lock_acquire (dir_lock);

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  //Check if safe to remove
  if (inode_is_dir (inode) && !dir_can_remove (inode)) {
    goto done;
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (current_dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  if (lock_held_by_current_thread (dir_lock)) 
  {
    lock_release (dir_lock);
  }

  if (!strcmp (name, CURRENT_DIRECTORY)) 
  {
    dir_close (current_dir);
  }
  
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. 
   While searching, no other opeeration can be performed on DIR*/
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct lock *dir_lock = get_dir_lock (dir_get_inode (dir));
  lock_acquire (dir_lock);
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          lock_release(dir_lock);
          return true;
        } 
    }

  lock_release (dir_lock);
  return false;
}

/* Directories opened using syscalls are stored as struct file pointers by
   the process that opened it. When switching between the stored struct
   file pointer and a created struct dir pointer, the position of the new
   dir* must match the position stored in the file*
   
   Updates the position of DIR to POS
   While updating the position, no other operations can be performed on DIR*/
void
dir_seek(struct dir *dir, off_t pos){
  ASSERT (dir != NULL);
  ASSERT (pos >= 0);
  
  struct lock *dir_lock = get_dir_lock(dir_get_inode(dir));
  lock_acquire(dir_lock);
  
  dir->pos = pos;
  
  lock_release(dir_lock);
}

/* Used to update the position of a file pointer stored by a user process
   Returns the position of DIR
   While accessing DIR, no other operation can be performed on DIR */
int
dir_tell (struct dir *dir)
{
  ASSERT (dir != NULL);

  struct lock *dir_lock = get_dir_lock (dir_get_inode (dir));
  lock_acquire (dir_lock);

  int position = dir->pos;

  lock_release (dir_lock);
  return position;
}
