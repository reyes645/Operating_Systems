#include <stdio.h>
#include "vm/swap.h"
#include "threads/malloc.h"

#define SECTORS_IN_PAGE 8

static struct swap_table *swap_table;

/*Initializes the swap table*/
void 
swap_init (void) 
{
    swap_table = malloc (sizeof (struct swap_table));

    struct block *swap_partition = block_get_role (BLOCK_SWAP);
    int num_pages = block_size (swap_partition) / SECTORS_IN_PAGE;
    
    swap_table->swap_map = bitmap_create (num_pages);
    swap_table->swap_partition = swap_partition;
}

/*Destroys the swap table, cleans out the swap, and frees all of 
  its data structures*/
void 
swap_destroy (void) 
{
    int limit = block_size (swap_table->swap_partition);
    int index;
    for (index = 0; index < limit; index++) 
    {
        block_write (swap_table->swap_partition, index, 0);
    }

    bitmap_destroy (swap_table->swap_map);
    free (swap_table);
}

/*Used when a page goes from memory to the swap
  Scans the bitmap to find and open spot in Swap and reads in the data
  stored at the page entry PAGE's mem_addr then calls page_set_sector to
  update PAGE with the new index*/
void 
swap_write (struct sp_entry *page)
{
    unsigned int index = 
        bitmap_scan_and_flip (swap_table->swap_map, 0, 1, false);
    if (index == BITMAP_ERROR)
    {
        PANIC("Swap is full\n");
    }
    
    unsigned int i;
    for (i = 0; i < SECTORS_IN_PAGE; i++)
    {
        void *address = page->mem_addr + (BLOCK_SECTOR_SIZE * i);
        int sector_index = (index * SECTORS_IN_PAGE) + i;
        block_write (swap_table->swap_partition, sector_index, address);
    }

    page_set_sector (page, index);
}

/*Used when a page moves out of Swap and into memory
  Copies the data stored at the page entry PAGE's swap_index into KPAGE one
  sector at a time. Then flips the swap_index bit in swap_map to show the 
  slot is free*/
void 
swap_read (struct sp_entry *page, void *kpage)
{
    unsigned int index = page->swap_index;
    int i;
    
    for (i = 0; i < SECTORS_IN_PAGE; i++)
    {
        void *address = kpage + (BLOCK_SECTOR_SIZE * i);
        int sector_index = (index * SECTORS_IN_PAGE) + i;
        block_read (swap_table->swap_partition, sector_index, address);
    }
    
    bitmap_flip (swap_table->swap_map, index);
}

/*Used when a dying thread still has a page stored in swap
  Flips PAGE's swap_index bit in the swap_map to show it is now unused*/
void 
swap_slot_clear (struct sp_entry *page) 
{
    int index = page->swap_index;
    bitmap_flip (swap_table->swap_map, index);
}
