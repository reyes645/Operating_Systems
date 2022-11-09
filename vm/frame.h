#include <stdbool.h>
#include "threads/thread.h"

struct frame_entry 
{
    void *upage;                /* Virtual address of page stored in frame*/
    struct thread *frame_owner; /* Thread that upage belongs to*/
    bool used;                  /* True if allocated, false otherwise*/
};

struct frame_entry **frame_table;

/*Global variables used by frame table*/
unsigned int table_size;
void *base_address;
unsigned int clock_hand;

void frame_init(void);
bool frame_allocate(void *upage, void *kpage);
bool frame_deallocate(void *kpage);
void frame_destroy(void);
