#include <inttypes.h>
#include <stdio.h>

#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include <debug.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

#define STACK_LIMIT (1 << 23)

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill(struct intr_frame *);
static void page_fault(struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
 * programs.
 *
 * In a real Unix-like OS, most of these interrupts would be
 * passed along to the user process in the form of signals, as
 * described in [SV-386] 3-24 and 3-25, but we don't implement
 * signals.  Instead, we'll make them simply kill the user
 * process.
 *
 * Page faults are an exception.  Here they are treated the same
 * way as other exceptions, but this will need to change to
 * implement virtual memory.
 *
 * Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
 * Reference" for a description of each of these exceptions. */
void
exception_init(void)
{
    /* These exceptions can be raised explicitly by a user program,
     * e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     * we set DPL==3, meaning that user programs are allowed to
     * invoke them via these instructions. */
    intr_register_int(3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
    intr_register_int(4, 3, INTR_ON, kill, "#OF Overflow Exception");
    intr_register_int(5, 3, INTR_ON, kill, "#BR BOUND Range Exceeded Exception");

    /* These exceptions have DPL==0, preventing user processes from
     * invoking them via the INT instruction.  They can still be
     * caused indirectly, e.g. #DE can be caused by dividing by
     * 0.  */
    intr_register_int(0,  0, INTR_ON, kill, "#DE Divide Error");
    intr_register_int(1,  0, INTR_ON, kill, "#DB Debug Exception");
    intr_register_int(6,  0, INTR_ON, kill, "#UD Invalid Opcode Exception");
    intr_register_int(7,  0, INTR_ON, kill, "#NM Device Not Available Exception");
    intr_register_int(11, 0, INTR_ON, kill, "#NP Segment Not Present");
    intr_register_int(12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
    intr_register_int(13, 0, INTR_ON, kill, "#GP General Protection Exception");
    intr_register_int(16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
    intr_register_int(19, 0, INTR_ON, kill, "#XF SIMD Floating-Point Exception");

    /* Most exceptions can be handled with interrupts turned on.
     * We need to disable interrupts for page faults because the
     * fault address is stored in CR2 and needs to be preserved. */
    intr_register_int(14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats(void)
{
    printf("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill(struct intr_frame *f)
{
    /* This interrupt is one (probably) caused by a user process.
     * For example, the process might have tried to access unmapped
     * virtual memory (a page fault).  For now, we simply kill the
     * user process.  Later, we'll want to handle page faults in
     * the kernel.  Real Unix-like operating systems pass most
     * exceptions back to the process via signals, but we don't
     * implement them. */

    /* The interrupt frame's code segment value tells us where the
     * exception originated. */
    thread_current ()->exit_status = -1;
    switch (f->cs) {
    case SEL_UCSEG:
        /* User's code segment, so it's a user exception, as we
         * expected.  Kill the user process.  */
        printf("%s: dying due to interrupt %#04x (%s).\n",
               thread_name(), f->vec_no, intr_name(f->vec_no));
        intr_dump_frame(f);
        thread_exit();

    case SEL_KCSEG:
        /* Kernel's code segment, which indicates a kernel bug.
         * Kernel code shouldn't throw exceptions.  (Page faults
         * may cause kernel exceptions--but they shouldn't arrive
         * here.)  Panic the kernel to make the point.  */
        intr_dump_frame(f);
        PANIC("Kernel bug - unexpected interrupt in kernel");

    default:
        /* Some other code segment?  Shouldn't happen.  Panic the
         * kernel. */
        printf("Interrupt %#04x (%s) in unknown segment %04x\n",
               f->vec_no, intr_name(f->vec_no), f->cs);
        thread_exit();
    }
}

// Use clock algorithm to choose a frame to evict,
// swap out to disk if not already in the file system, and
// clear from the page table and frame table
// Returns the pointer to the page evicted
static void 
*evict (void)
{
  struct frame_entry *frame = frame_table[clock_hand];
  uint32_t *pd = frame->frame_owner->pagedir;
  // Choose a frame to evict
  while (pagedir_is_accessed(pd, frame->upage))
  {
    pagedir_set_accessed (pd, frame->upage, false);
    clock_hand = (clock_hand < (table_size - 1)) ? clock_hand + 1 : 0;
    frame = frame_table[clock_hand];
    pd = frame->frame_owner->pagedir;
  }
  
  // Calculate address of page in physical memory
  void *kpage = (clock_hand * PGSIZE) + base_address;
  
  struct sp_entry *page = page_find (frame->frame_owner, frame->upage);
  // Send contents of kpage into the swap partition
  if (pagedir_is_dirty (pd, frame->upage))
  {
    swap_write(page);
  } else 
  {
    // This data already exists in the file system, simply
    // update the SPT
    page_replace (page, NULL, BLOCK_FILESYS);
  }
  // Update VM data structures
  pagedir_clear_page (pd, page->upage);
  frame_deallocate (kpage);
  // Zero the page and return to page fault handler
  memset (kpage, 0, PGSIZE);
  return kpage;
}
// Memory access has been determined to need stack growth,
// Add a new entry in both the PT and SPT for virtual
// address UPAGE and physical memory address KPAGE
// Return true if successful
static bool
grow_stack(void *fault_addr, struct sp_entry *page, void *upage, void *kpage)
{
  bool success = false;
  // Check we have not gone over the 8MB stack limit
  if (fault_addr < (PHYS_BASE - STACK_LIMIT)) {
    thread_current ()->exit_status = -1; 
    thread_exit ();
  }
  // Update page table
  success = install_page (upage, kpage, true);
  pagedir_set_dirty (thread_current ()->pagedir, upage, true);
  // Update SPT
  if (success)
  {
    page = page_insert (upage, kpage, BLOCK_KERNEL);
    success = (page != NULL);
    page_set_writable (page, true);
  }
  return success;
}
// Memory access is from file system, read file into physical
// memory address KPAGE
static void
read_filesys (struct sp_entry *page, void *kpage)
{
  struct file *file = page->file_addr;
  bool lock_held_previous = lock_held_by_current_thread (&filesys_lock);
  if (!lock_held_previous)
  {
    lock_acquire (&filesys_lock);
  }
  // Read file without changing its position
  file_read_at (file, kpage, page->read_bytes, file_tell(file));
  if (!lock_held_previous)
  {
    lock_release (&filesys_lock);
  }
}


/* Page fault handler.  This is a skeleton that must be filled in
 * to implement virtual memory.  Some solutions to project 2 may
 * also require modifying this code.
 *
 * At entry, the address that faulted is in CR2 (Control Register
 * 2) and information about the fault, formatted as described in
 * the PF_* macros in exception.h, is in F's error_code member.  The
 * example code here shows how to parse that information.  You
 * can find more information about both of these in the
 * description of "Interrupt 14--Page Fault Exception (#PF)" in
 * [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault(struct intr_frame *f)
{
    bool not_present; /* True: not-present page, false: writing r/o page. */
    bool write;       /* True: access was write, false: access was read. */
    bool user;        /* True: access by user, false: access by kernel. */
    void *fault_addr; /* Fault address. */

    /* Obtain faulting address, the virtual address that was
     * accessed to cause the fault.  It may point to code or to
     * data.  It is not necessarily the address of the instruction
     * that caused the fault (that's f->eip).
     * See [IA32-v2a] "MOV--Move to/from Control Registers" and
     * [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     * (#PF)". */
    asm ("movl %%cr2, %0" : "=r" (fault_addr));

    /* Turn interrupts back on (they were only off so that we could
     * be assured of reading CR2 before it changed). */
    intr_enable();

    lock_acquire (&vm_lock);

    /* Count page faults. */
    page_fault_cnt++;

    /* Determine cause. */
    not_present = (f->error_code & PF_P) == 0;
    write = (f->error_code & PF_W) != 0;
    user = (f->error_code & PF_U) != 0;

    // Get the SPT entry for this faulting address
    void *upage = fault_addr - ((unsigned int) fault_addr % PGSIZE);
    struct sp_entry *page = page_find (thread_current (), upage);

    bool success = false;
    // Determine if stack growth
    bool stack_growth = (fault_addr >= (f->esp - PUSH_BYTES)) && !page;
    
    // Validate the faulting address, address is invalid if writing to a read
    // only page, if it is from the kernel pool of virtual memory or
    // the page does not exist in the SPT and is not stack growth
    if (!not_present || !is_user_vaddr (upage) || (!page && !stack_growth)) {
        thread_current ()->exit_status = -1;
        thread_exit ();
    }

    void *kpage = palloc_get_page (PAL_USER | PAL_ZERO);

    // No more pages left, must choose a page to evict  
    if (!kpage)
    {
        kpage = evict ();
    }

    // Grow the stack
    if (stack_growth)
    {
        success = grow_stack (fault_addr, page, upage, kpage);
    }else {
        // Normal memory access, must bring in data from somewhere on disk
        bool swapped = false;
        // Read from the file system
        if (page->block == BLOCK_FILESYS)
        {
        read_filesys (page, kpage);
        
        }else if (page->block == BLOCK_SWAP)
        {
        // Read from the swap partition
        swap_read (page, kpage);
        swapped = true;
        }
        // Update the PT and SPT
        success = install_page (upage, kpage, page->writable)
                && page_replace (page, kpage, BLOCK_KERNEL);

        // Set this page back to dirty if brought in from swap
        if (swapped)
        {
        pagedir_set_dirty (thread_current ()->pagedir, upage, true);
        }
    }

    // Memory failure, free resources and exit
    if (!success) 
    {
        palloc_free_page (kpage);
        pagedir_clear_page(thread_current ()->pagedir, upage);
        thread_current ()->exit_status = -1;
        thread_exit ();
    }
    lock_release (&vm_lock);
}


