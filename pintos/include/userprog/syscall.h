#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>

void syscall_init (void);

void sys_halt (void);
void sys_exit (int status);
bool sys_create (const char *file, unsigned initial_size);
int sys_open (const char *file);
int sys_filesize (int fd);
int sys_read (int fd, const void *buffer, unsigned size);
int sys_write (int fd, const void *buffer, unsigned size);
void sys_close(int fd);

#endif /* userprog/syscall.h */
