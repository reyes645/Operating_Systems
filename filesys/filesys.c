#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();
  // set main thread's cwd to the root
  thread_current ()->cwd = dir_open_root ();
  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char *file_name;

  // get the parent directory in which to add the file
  struct dir *dir = parse_path (name, &file_name);

  // cannot create a file name with names ".", "..", or "/"
  if (!strcmp (file_name, CURRENT_DIRECTORY)
      || !strcmp (file_name, PARENT_DIRECTORY)
      || !strcmp (file_name, ROOT)
      || strlen (name) == 0)
  {
    return false;
  }

  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, 
                                  inode_get_inumber (dir_get_inode(dir)), 0)
                  && dir_add (dir, file_name, inode_sector));

  if (!success && inode_sector != 0) 
  {
    free_map_release (inode_sector, 1);
  }

  free (file_name);
  dir_close (dir);
  
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
  char *file_name;
  // get parent directory that the file exists in
  struct dir *dir = parse_path (name, &file_name);
  struct inode *inode = NULL;

  if (dir != NULL) {
    // file name is "..", we want the parent of whatever dir is
    if (!strcmp (file_name, PARENT_DIRECTORY)) 
    {
      inode = inode_open (inode_get_parent (dir_get_inode (dir)));
      dir_close (dir);

    } else if (!strcmp (file_name, CURRENT_DIRECTORY)
                || !strcmp (file_name, ROOT)) 
    {
      // if a single dot or the root, simply get dir
      inode = dir_get_inode (dir);

    } else
    {
      // else look for the file name in dir
      dir_lookup (dir, file_name, &inode);
      dir_close (dir);
    }

    free (file_name);
  }

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char *file_name;
  // get the parent directory and look there for the file
  struct dir *dir = parse_path (name, &file_name);
  bool success = dir != NULL && dir_remove (dir, file_name);
  free (file_name);
  dir_close (dir);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, 0))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

// find the number of files and directories in the path string
static int 
get_num_tokens (const char *path, char *delimiter) 
{
  char *path_copy = malloc (strlen (path) + 1);
  if (!path_copy) 
  {
    return -1;
  }
  
  strlcpy (path_copy, path, strlen (path) + 1);
  char *token;
  char *save_ptr;

  int num_tokens = 0;
  for (token = strtok_r (path_copy, delimiter, &save_ptr); token != NULL;
        token = strtok_r (NULL, delimiter, &save_ptr)) 
  {
    num_tokens++;
  }

  free (path_copy);
  return num_tokens;
}

// store the tokenized path into an array of strings
static char **
get_paths (const char *path, int num_tokens, char *delimiter) 
{
  char *path_copy = malloc (strlen (path) + 1);
  if (!path_copy) 
  {
    return NULL;
  }
  strlcpy (path_copy, path, strlen (path) + 1);
  
  char **paths = malloc (sizeof (char *) * num_tokens);
  if (!paths) 
  {
    free (path_copy);
    return NULL;
  }

  char *save_ptr_two;
  int index;
  char *token = strtok_r (path_copy, delimiter, &save_ptr_two);
  paths[0] = malloc (strlen (token) + 1);
  strlcpy (paths[0], token, strlen (token) + 1);

  for (index = 1; index < num_tokens; index++) 
  {
    token = strtok_r (NULL, delimiter, &save_ptr_two);
    paths[index] = malloc (strlen (token) + 1);
    strlcpy (paths[index], token, strlen (token) + 1);
  }

  free (path_copy);
  return paths;
}

/* Frees path and all the strings it stores*/
static void
free_paths_helper (char ** paths, int num_tokens)
{
  int index;
  for (index = 0; index < num_tokens; index++) 
  {
    free (paths[index]);
  }

  free (paths);
}

// parses a path by the slash character, returns the parent directory
// of the last thing in the path which must be a file name.
// Fills FILE_NAME with the last string in the path
// caller must close the dir returned in this function
struct dir* 
parse_path (const char *path, char **file_name) 
{
  char *delimiter = "/";
  int num_tokens = get_num_tokens (path, delimiter);
  if (num_tokens == -1) 
  {
    return NULL;
  }

  // if path is simply a single slash return the root
  if (!strcmp (path, ROOT)) 
  {
    *file_name = malloc (strlen (path) + 1);
    strlcpy (*file_name, path, strlen (path) + 1);
    return dir_open_root ();
  }

  char **paths = get_paths (path, num_tokens, delimiter);
  if (!paths) 
  {
    return NULL;
  }

  // if the first character is a slash start at root, otherwise
  // start with the current working directory
  struct dir *current_dir = (path[0] == '/') ? dir_open_root ()
                            : dir_reopen (thread_current ()->cwd);
  int current_index = 0;
  int tokens = num_tokens;
  // parse path num_tokens - 1 times to find the directory of the
  // second to last string in the path
  while (tokens > 1)
  {
    // open parent if a double dot is found
    if (!strcmp (paths[current_index], PARENT_DIRECTORY))
    {
      struct dir *parent = dir_open_parent (current_dir);
      if (!parent)
      {
        free_paths_helper (paths, num_tokens);
        return NULL;
      }

      dir_close (current_dir);
      current_dir = parent;

    } else if (strcmp (paths[current_index], CURRENT_DIRECTORY))
    {
      // if string is anything but a single dot, continue parsing
      // if a single dot, then nothing is done
      struct inode *inode = malloc (sizeof (struct inode*));
      // lookup the current string in the current parent
      // make sure this new inode is also a directory, cannot
      // have single files in the middle of the path
      if (!inode || !dir_lookup (current_dir, paths[current_index], &inode)
          || !inode_is_dir (inode))
      {
        free_paths_helper (paths, num_tokens);
        return NULL;
      }

      dir_close (current_dir);
      current_dir = dir_open (inode);
    }
    current_index++;
    tokens--;
  }

  // copy the final string into file_name
  *file_name = malloc (strlen (paths[current_index]) + 1);
  if (!file_name)
  {
    free_paths_helper (paths, num_tokens);
    return NULL;
  }

  strlcpy (*file_name, paths[current_index], strlen (paths[current_index]) + 1);

  // free resources
  free_paths_helper(paths, num_tokens);
  return current_dir;
}
