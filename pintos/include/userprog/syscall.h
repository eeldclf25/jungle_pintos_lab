#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);

void sys_halt (void);
void sys_exit (int status);
int sys_write (int fd, const void *buffer, unsigned length);
bool sys_create (const char *file, unsigned initial_size);

#endif /* userprog/syscall.h */
