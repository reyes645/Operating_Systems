#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <stdbool.h>
#include "devices/block.h"
#include "threads/thread.h"

struct sp_entry 
{
    void *upage;                    /* Virtual address of page; Key for hash*/
    enum block_type block;          /* Location of page*/ 
    void *mem_addr;                 /* Address of page if in memory*/
    struct file *file_addr;         /* Pointer to access file in filesys*/
    int read_bytes;                 /* Number of bytes read in from file*/
    int swap_index;                 /* Index of page if in Swap*/
    bool writable;                  /* True if writable*/
    struct hash_elem sup_elem;      /* Element used in sup_table*/
};

void page_init(struct hash *sup_table);
struct sp_entry *page_find(struct thread *thread, void *upage);
struct sp_entry *page_insert(void *upage, void *address, enum block_type block);
bool page_replace(struct sp_entry *page, void *new_address,
                    enum block_type block);
void page_set_writable(struct sp_entry *page, bool writable);
void page_set_rb(struct sp_entry *page, int read_bytes);
void page_set_sector(struct sp_entry *page, int index);
void page_table_destroy(void);

#endif
