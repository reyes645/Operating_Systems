#include <stdio.h>
#include <syscall-nr.h>
#include <stdbool.h>	
#include <string.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/inode.h"		
#include "filesys/directory.h"		
#include "filesys/free-map.h"
#include "threads/malloc.h"


static void syscall_handler(struct intr_frame *);

// All system calls
static void halt_call (struct intr_frame *f, void **esp);
static void exit_call (struct intr_frame *f, void **esp);
static void exec_call (struct intr_frame *f, void **esp);
static void wait_call (struct intr_frame *f, void **esp);
static void create_call (struct intr_frame *f, void **esp);
static void remove_call (struct intr_frame *f, void **esp);
static void open_call (struct intr_frame *f, void **esp);
static void filesize_call (struct intr_frame *f, void **esp);
static void read_call (struct intr_frame *f, void **esp);
static void write_call (struct intr_frame *f, void **esp);
static void seek_call (struct intr_frame *f, void **esp);
static void tell_call (struct intr_frame *f, void **esp);
static void close_call (struct intr_frame *f, void **esp);
static void chdir_call (struct intr_frame *f, void **esp);	
static void mkdir_call (struct intr_frame *f, void **esp);	
static void readdir_call (struct intr_frame *f, void **esp);	
static void isdir_call (struct intr_frame *f, void **esp);	
static void inumber_call (struct intr_frame *f, void **esp);

static int init_file (struct file *file);
static void *get_argument (void **esp);

// Array of function pointers
static void (*system_calls[]) (struct intr_frame*, void**) = 
{
    halt_call, exit_call, exec_call, wait_call, create_call,
    remove_call, open_call, filesize_call, read_call, write_call,
    seek_call, tell_call, close_call, NULL, NULL, chdir_call,	
    mkdir_call, readdir_call, isdir_call, inumber_call
};

void
syscall_init(void)
{
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

// Validate a pointer, must not be NULL, be in kernel space, or 
// be in unmapped memory
static void
check_pointer (const char *argument)
{
  uint32_t *page = thread_current ()->pagedir;
  if (argument == NULL || !is_user_vaddr (argument)
        || !pagedir_get_page (page, argument))
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
}

// When there is a system call, dispatch to a system call
// handler which runs the approriate function
static void
syscall_handler(struct intr_frame *f)
{
  check_pointer(f->esp);
  void *esp = f->esp;

  int call_number = *(int *) esp;
  esp += sizeof(int);
  
  // Make sure call number is valid
  if (call_number >= 0 && call_number < NUM_SYS_CALLS) 
  {
    (*system_calls[call_number])(f, &esp);
  }
}

// Generate a file descriptor for a file and place into
// the array of files at that index. Change the index
// of the next_fd to the next available position in the
// array. If an error(such as no space left), return -1
static int
init_file (struct file *file) 
{
  int fd = thread_current ()->next_fd;

  if (fd > 1) 
  {
    int new_fd = fd;
    
    // Place file into array
    thread_current ()->files[new_fd] = file;
    struct file *current_file = thread_current ()->files[new_fd];

    //Search for the next open index
    while (current_file != NULL && new_fd < MAX_FILES - 1) 
    {
      new_fd++;
      current_file = thread_current ()->files[new_fd];
    }

    //Max number of files already open
    if (fd == MAX_FILES) 
    {
      thread_current ()->next_fd = -1;
      return -1;
    }
    
    // Store next fd index and return file descriptor for
    // this new file
    thread_current ()->next_fd = new_fd;
    return fd;
  }
  
  return -1;
}

/* Returns true if the file stored at fd is a directory*/	
static bool	
is_directory (int fd)	
{	
  struct inode *inode = file_get_inode (thread_current ()->files[fd]);	
  return inode_is_dir (inode);	
}

/*Get the next four bytes from the stack and return them as a void pointer
  the system calls will cast to the correct type*/
static void 
*get_argument (void **esp) 
{
  void *argument = *esp;
  check_pointer (argument);
  *esp += sizeof(void *);
  return argument;
}

// Shut down the system
static void
halt_call (struct intr_frame *f UNUSED, void **esp UNUSED)
{
  shutdown_power_off ();
}

// Terminate the current process and return the exit status
// so the parent can collect it
static void
exit_call (struct intr_frame *f UNUSED, void **esp)
{
  int status = *(int *) get_argument (esp);
  thread_current ()->exit_status = status;
  thread_exit ();
}

// Execute the program under the name of the command line
// passed. Return the tid of the process being executed
static void
exec_call (struct intr_frame *f, void **esp) 
{
  const char *cmd_line = *(char **) get_argument (esp);
  check_pointer (cmd_line);
  f->eax = process_execute (cmd_line);
}

// Wait for the child process of given tid to finish
// Return exit status of child
static void 
wait_call (struct intr_frame *f, void **esp)
{
  tid_t child = *(tid_t *) get_argument (esp);
  f->eax = process_wait (child);
}

// Create a new file with given file_name and size
// Return TRUE is successfully created, FALSE if not
static void
create_call (struct intr_frame *f, void **esp)
{
  const char *file_name = *(char **) get_argument (esp);
  check_pointer (file_name);
  size_t initial_size = *(size_t *) get_argument (esp);

  f->eax = filesys_create (file_name, initial_size);
}

// Remove file from file system, without closing
// any instances held by a process
// Return TRUE is successfully removed, FALSE if not
static void 
remove_call (struct intr_frame *f, void **esp)
{
  const char *file_name = *(char **) get_argument (esp);
  check_pointer (file_name);

  f->eax = filesys_remove (file_name);
}

// Open a file, assign it a file descriptor, and place
// file into thread's open file array
// Return file descriptor, or -1 if error
static void
open_call (struct intr_frame *f, void **esp)
{
  const char *file_name = *(char **) get_argument (esp);
  check_pointer (file_name);

  struct file *file = filesys_open (file_name);
  f->eax = (file) ? init_file (file) : -1;
}

// Return the size of a file in bytes
static void
filesize_call (struct intr_frame *f, void **esp)
{
  int fd = *(int *) get_argument (esp);
  
  // Check if valid file descriptor
  if (fd < 2 || fd >= MAX_FILES) {
    thread_current ()->exit_status = -1;	
    thread_exit ();	
  }else if (is_directory (fd))	
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }

  struct file *file = thread_current ()->files[fd];

  if (file)
  {
    f->eax = file_length (file);
  } else
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
}

// If fd is 0, then read size bytes from stdin
static void
read_from_stdin(struct intr_frame *f, off_t size) 
{
  int counter = 0;
  while (counter < size) 
    {
      input_getc ();
      counter++;
    }

    // Return the number of bytes read
    f->eax = counter;
}

// Read size bytes from an open file and return the number of
// bytes actually read
static void
read_call (struct intr_frame *f, void **esp)
{
  int fd = *(int *) get_argument (esp);

  // Check if valid file descriptor
  if (fd == STDOUT_FILENO || fd < 0 || fd >= MAX_FILES) 
  {
    f->eax = -1;
  } else
  {
    void *buffer = *(void **) get_argument (esp);
    check_pointer ((char *) buffer);
    off_t size = *(off_t *) get_argument (esp);
    check_pointer ((char *) (buffer + size));

    // read from standard input
    if (fd == STDIN_FILENO)
    {
      read_from_stdin(f, size);
      
    } else
    {
      // read from file with given fd
      struct file *file = thread_current ()->files[fd];

      // check if valid file
      if (file && !is_directory (fd))
      {
        f->eax = file_read (file, buffer, size); 
      } else
      {
        f->eax = -1;
      }
    }
  }
}

// If fd is 1, then write to stdout 256 bytes at a time
static void
write_to_stdout(struct intr_frame *f, off_t old_size, void *buffer)
{
  off_t size = old_size;
  while (size / BUFFER_LIMIT >= 1) 
    {
      putbuf (buffer, size);
      size -= BUFFER_LIMIT;
      buffer += BUFFER_LIMIT;
    }

  // Write any remaining bytes
  if (size % BUFFER_LIMIT > 0)
    {
      putbuf (buffer, size);
    }
  
  // Return the number of bytes written
  f->eax = old_size;
}

/* Write size bytes into an open file and return the number of bytes
   actually written*/
static void
write_call (struct intr_frame *f, void **esp)
{
  int fd = *(int *) get_argument (esp);
  if (fd <= STDIN_FILENO || fd >= MAX_FILES) 
  {
    f->eax = -1;
  } else
  {
    char *buffer = *(char **) get_argument (esp);
    check_pointer (buffer);
    off_t size = *(off_t *) get_argument (esp);
    check_pointer ((char *) (buffer + size));
    // write to stdout
    if (fd == STDOUT_FILENO) 
    {
      write_to_stdout(f, size, buffer);
    } else
    {
      // write to file given by fd
      struct file *file = thread_current ()->files[fd];

      // check if valid file
      if (file)
      {
        f->eax = file_write (file, buffer, size);
      } else
      {
        f->eax = -1;
      }
    }
  }
}

/*Change the next byte in a open file to be read or written to position*/
static void
seek_call (struct intr_frame *f UNUSED, void **esp)
{
  int fd = *(int *) get_argument (esp);

  //Check if valid file descriptor
  if (fd < 2 || fd >= MAX_FILES)
  {	
    thread_current ()->exit_status = -1;	
    thread_exit ();	
  }else if (is_directory (fd))
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }

  off_t position = *(off_t *) get_argument (esp);

  struct file *file = thread_current ()->files[fd];
  // Check if valid file
  if (!file)
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }

  file_seek (file, position);
}

/*Returns the position of the next byte in an open file to be read or written*/
static void
tell_call (struct intr_frame *f, void **esp)
{
  int fd = *(int *) get_argument (esp);

  //Check if valid file descriptor
  if(fd < 2 || fd >= MAX_FILES)
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }else if (is_directory (fd))	
  {	
    thread_current ()->exit_status = -1;	
    thread_exit ();
  }
  
  struct file *file = thread_current ()->files[fd];
  // Check if valid file
  if (file)
  {
    f->eax = file_tell (file);
  } else
  {
    f->eax = -1;
  }
}

/*Close an open file represented by fd, only closes that specific instance and
  removes it from the list*/
  static void
close_call (struct intr_frame *f UNUSED, void **esp)
{
  int fd = *(int *) get_argument (esp);

  //Check if valid file descriptor
  if(fd < 2 || fd >= MAX_FILES)
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }

  struct file *file = thread_current ()->files[fd];
  if (!file)
  {
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
  
  file_close (file);
  thread_current ()->files[fd] = NULL;

  //Set next_fd to newly opened
  if (fd < thread_current ()->next_fd) 
  {
    thread_current ()->next_fd = fd;
  }
}

		
/* Changes a thread's current working directory to DIR.		
   Accepts absolute and relative paths and the special 		
   directory names "/", ".", and ".."		
   Returns true if successful, false otherwise*/		
static void 		
chdir_call (struct intr_frame *f, void **esp) 		
{		
  char *dir = *(char **) get_argument (esp);		
  check_pointer (dir);		
  		
  char *dir_name;		
  struct dir *directory = parse_path(dir, &dir_name);		
  if (!directory) {		
    f->eax = 0;		
  } else {  		
    bool success = false;		
    if (!strcmp (dir_name, ROOT) || !strcmp (dir_name, CURRENT_DIRECTORY)) 		
    {		
      //Change cwd to directory		
      dir_close (thread_current ()->cwd);		
      thread_current ()->cwd = directory;		
      success = true;		
    } else if (!strcmp (dir_name, PARENT_DIRECTORY))		
    {		
      //Change cwd to directory's parent directory		
      dir_close (thread_current ()->cwd);		
      thread_current ()->cwd = dir_open_parent (directory);		
      dir_close (directory);		
      success = true;		
      		
    } else {		
      //Find the directory named DIR_NAME stored in DIRECTORY and make it cwd		
      struct inode *inode;		
      success = dir_lookup (directory, dir_name, &inode)		
                  && inode_is_dir (inode);		
      if (success) 		
      {		
        dir_close (thread_current ()->cwd);		
        thread_current ()->cwd = dir_open (inode);		
      }	
      dir_close (directory);	
    }	
    	
    free (dir_name);	
    f->eax = success;	
  }	
}	

/* Make a new directory named DIR	
  Accepts absolute and relative paths and paths using the special names	
  "/", ".", and ".." although it cannot make a directory with those names.	
  If using a relative path, the new directory is created as a subdirectory	
  of the cwd else it is the subdirectory of the directory found by parse_path.	
  Returns true if successful, false otherwise*/	
static void 	
mkdir_call (struct intr_frame *f, void **esp) 	
{	
  char *dir = *(char **) get_argument (esp);	
  check_pointer (dir);	
  char *new_directory;	
  struct dir *directory = parse_path (dir, &new_directory);	
  if (!directory) 	
  {	
    f->eax = 0;	
    	
  } else 	
  {	
    block_sector_t inode_sector = 0;	
    bool success = free_map_allocate (1, &inode_sector)	
                    && dir_add (directory, new_directory, inode_sector)	
                    && dir_create (inode_sector, 0,	
                        inode_get_inumber (dir_get_inode (directory)));	
    if (!success && inode_sector != 0)	
    {	
      free_map_release (inode_sector, 1);	
    }	
    free (new_directory);	
    dir_close (directory);	
    f->eax = success;	
  }	
}	

/* Searches the directory known by FD for the next used directory entry.	
   If a directory entry is found, return true and copy it's name into the	
   NAME buffer. Else return false. 	
   FD must refer to a directory*/	
static void 	
readdir_call (struct intr_frame *f, void **esp) 	
{	
  int fd = *(int *) get_argument (esp);	
  if (fd < 2 || fd >= MAX_FILES)	
  {	
    f->eax = 0;	
  }else if (!is_directory (fd))	
  {	
    f->eax = 0;	
  }else	
  {	
    char *name = *(char **) get_argument (esp);	
    check_pointer(name);	
    struct file *file = thread_current ()->files[fd];	
    struct dir *dir = dir_open (file_get_inode (file));	
    if (!dir)	
    {	
      f->eax = 0;	
    }else	
    {	
      dir_seek (dir, file_tell (file));	
      bool success = dir_readdir (dir, name);	
      if (success)	
      {	
        file_seek (file, dir_tell (dir));   	
      }	
      free (dir);	
      f->eax = success;	
    }	
  }	
}	

// return true if the file represented by fd is a directory,	
// false otherwise	
static void 	
isdir_call (struct intr_frame *f, void **esp)	
{	
  int fd = *(int *) get_argument (esp);	
  if (fd < 2 || fd >= MAX_FILES)	
  {	
    f->eax = 0;	
  }else	
  {	
    f->eax = is_directory (fd);	
  }	
}	

// return the inode number of the file or directory represented	
// by fd	
static void 	
inumber_call (struct intr_frame *f, void **esp) 	
{	
  int fd = *(int *) get_argument (esp);	
  if (fd < 2 || fd >= MAX_FILES)	
  {	
    thread_current()->exit_status = -1;	
    thread_exit ();	
  }	
  struct file *file = thread_current ()->files[fd];	
  f->eax = inode_get_inumber (file_get_inode (file));	
}
