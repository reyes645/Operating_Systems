#include <bitmap.h>
#include "devices/block.h"
#include "vm/page.h"

struct swap_table 
{
    struct bitmap *swap_map;        /* Tracks which swap slots are used*/
    struct block *swap_partition;   /* Points to BLOCK_SWAP*/
};

void swap_init(void);
void swap_destroy(void);
void swap_write(struct sp_entry *page);
void swap_read(struct sp_entry *page, void *kpage);
void swap_slot_clear(struct sp_entry *page);
