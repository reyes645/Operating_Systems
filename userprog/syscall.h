#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

#define NUM_SYS_CALLS 13        /* number of system calls */
#define BUFFER_LIMIT 256        /* number of bytes to write to stdout*/

#endif /* userprog/syscall.h */
