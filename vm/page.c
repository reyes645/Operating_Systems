#include <stdio.h>
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"

unsigned page_hash (const struct hash_elem *element, void *aux);
bool page_hash_less (const struct hash_elem *a, const struct hash_elem *b,
                        void *aux);
void page_hash_action (struct hash_elem *element, void *aux);

/*Creates a hash table to store all of a thread's supplemental page
  table entries using each entry's upage as the key*/
void
page_init (struct hash *sup_table) 
{
    hash_init (sup_table, page_hash, page_hash_less, NULL);
}

/*Iterates through the supplemental page table of THREAD to find 
  the entry containing UPAGE
  Returns pointer to entry or NULL if no entry is found*/
struct sp_entry
*page_find (struct thread *thread, void *upage) 
{
    struct hash_elem *e;
    struct sp_entry page;
    page.upage = upage;
    e = hash_find (&thread->sup_table, &page.sup_elem);
    return (e != NULL) ? hash_entry (e, struct sp_entry, sup_elem) : NULL;
}

/*Creates a new page entry for the supplemental page table. If
  BLOCK is BLOCK_KERNEL, ADDRESS is stored in mem_addr. Else if
  BLOCK is BLOCK_FILESYS, ADDRESS is stored in file_addr.
  BLOCK can never be BLOCK_SWAP as nothing immediately goes to swap
  Returns pointer to new page entry*/
struct sp_entry
*page_insert (void *upage, void *address, enum block_type block) 
{
    struct sp_entry *entry = malloc (sizeof (struct sp_entry));
    if (entry == NULL) 
    {
        return NULL;
    }
    
    entry->upage = upage;
    entry->block = block;
    entry->swap_index = -1;

    if (block == BLOCK_FILESYS)
    {
        entry->file_addr = (struct file *) address;
    }else
    {
        entry->mem_addr = address;
    }
    
    hash_insert (&thread_current ()->sup_table, &entry->sup_elem);
    return entry;
}

/*Called when a page is being moved between memory and the file system
  Updates PAGE supplemental page table entry with the new location BLOCk and 
  address NEW_ADRESS that the page has been moved to
  Returns true if PAGE is updated successfully, false otherwise*/
bool 
page_replace (struct sp_entry *page, void *new_address, enum block_type block) 
{
    if (page != NULL) 
    {
        page->mem_addr = new_address;
        page->swap_index = -1;
        page->block = block;
        return true;
    }
    return false;
}

/*Sets supplemental page entry PAGES's writable value to WRITABLE*/
void 
page_set_writable (struct sp_entry *page, bool writable) 
{
    if (page != NULL) 
    {
        page->writable = writable;
    }
}

/*Sets supplemental page entry PAGES's read_bytes value to READ_BYTES*/
void 
page_set_rb (struct sp_entry *page, int read_bytes) 
{
    if (page != NULL)
    {
        page->read_bytes = read_bytes;
    } 
}

/*Called when a page contained by PAGE moves from memory to the swap
  INDEX is the index at which the memory has been written into swap.
  Updates the rest of PAGE's values*/
void 
page_set_sector (struct sp_entry *page, int index) 
{
    page->swap_index = index;
    page->block = BLOCK_SWAP;
    page->mem_addr = NULL;
}

/*Destroys the supplemental page table at the end of the current
  thread's life. Uses page_hash_action to free each entry and
  reclaim it's memory*/
void 
page_table_destroy (void) 
{
    struct hash sup_table = thread_current ()->sup_table;
    hash_destroy (&sup_table, page_hash_action);
}

/*Hash function used by the supplemental page table.
  Uses the page entry's upage as the key*/
unsigned 
page_hash (const struct hash_elem *element, void *aux UNUSED) 
{
    struct sp_entry *page = hash_entry (element, struct sp_entry, sup_elem);
    return hash_bytes (&page->upage, sizeof (void *));
}

/*Hash function used to sort entries in the supplemental page table.
  Entries are sorted in order of upage*/
bool 
page_hash_less(const struct hash_elem *a, const struct hash_elem *b,
                        void *aux UNUSED) 
{
    struct sp_entry *page = hash_entry (a, struct sp_entry, sup_elem);
    struct sp_entry *page_two = hash_entry (b, struct sp_entry, sup_elem);
    return (unsigned int) page->upage < (unsigned int) page_two->upage;
}

/*Hash function used to free an individual page table entry at the end of
  the current thread's life cycle. If the page stored in entry is currently
  in memory, it is freed and the frame table is updated. Else if the page is
  in swap, it's slot is cleared and the swap table updated*/
void 
page_hash_action (struct hash_elem *element, void *aux UNUSED) 
{
    struct sp_entry *page = hash_entry (element, struct sp_entry, sup_elem);
    if (page->block == BLOCK_KERNEL)
    {
        palloc_free_page (page->mem_addr);
        pagedir_clear_page (thread_current ()->pagedir, page->upage);
        frame_deallocate (page->mem_addr);

    }else if (page->block == BLOCK_SWAP)
    {
        swap_slot_clear (page);
    }

    free (page);
}

