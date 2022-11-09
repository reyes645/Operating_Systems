#include <stdio.h>
#include <string.h>
#include "vm/frame.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"

/*Initializes the frame table
  Counts the number of frames available in user memory and allocates the
  frame table accordingly.
  Also finds the starting address in physical memory where user memory is
  stored and sets the clock hand used for eviction to 0*/
void 
frame_init (void) 
{
    void *pages = palloc_get_page (PAL_USER | PAL_ZERO);
    base_address = pages;
    size_t num_frames = 1;

    while (palloc_get_page (PAL_USER | PAL_ZERO)) 
    {
        num_frames++;
    }

    palloc_free_multiple (pages, num_frames);
    frame_table = malloc (sizeof (struct frame_entry *) * num_frames);

    unsigned int index;
    for (index = 0; index < num_frames; index++) 
    {
        frame_table[index] = malloc (sizeof (struct frame_entry));
    }
    
    table_size = num_frames;
    clock_hand = 0;
}

/*Returns the frame entry representing the physical address KPAGE
  Returns NULL if KPAGE is invalid*/
static struct frame_entry 
*get_frame (void *kpage) 
{
    unsigned int address = (unsigned int) (kpage - base_address);
    address /= PGSIZE;

    if (address > table_size) 
    {
        return NULL;
    }

    return frame_table[address];
}

/*Updates the frame table entry found using KPAGE to show
  that it is being allocated the user virtual page UPAGE
  Returns true if successful, false otherwise*/
bool 
frame_allocate (void *upage, void *kpage) 
{  
    struct frame_entry *frame = get_frame (kpage);
    if (frame) 
    {
        frame->upage = upage;
        frame->frame_owner = thread_current ();
        return true;
    }
    return false;
}

/*Update the frame table entry found using KPAGE to show
  that it is no longer allocated
  Returns true if successful, false otherwise*/
bool 
frame_deallocate (void *kpage) 
{
    struct frame_entry *frame = get_frame (kpage);
    if (frame) 
    {
        frame->upage = NULL;
        frame->frame_owner = NULL;
        return true;
    }
    return false;
}

/*Destroys the frame table and frees all of it's entries*/
void 
frame_destroy (void) 
{
    unsigned int index;
    for (index = 0; index < table_size; index++) 
    {
        free(frame_table[index]);
    }

    free (frame_table);
}
