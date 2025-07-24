#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 시스템 콜 인자로 전달된 유저 포인터가 가리키고 있는 주소가 유효 한지 확인합니다.
	커널 주소 영역이거나 유저 페이지 테이블에 매핑 되지 않은 주소 라면 종료(exit(-1)) 시킵니다. */
void
check_address (void *addr) {
	if (is_kernel_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == 0) {
		sys_exit (-1);
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	switch (f->R.rax)
	{
		case SYS_HALT:
			sys_halt ();
			break;
		case SYS_EXIT:
			sys_exit (f->R.rdi);
			break;
		case SYS_FORK:
			break;
		case SYS_EXEC:
			break;
		case SYS_WAIT:
			break;
		case SYS_CREATE:
			sys_create (f-> R.rdi, f -> R.rsi);
			break;
		case SYS_REMOVE:
			break;
		case SYS_OPEN:
			break;
		case SYS_FILESIZE:
			break;
		case SYS_READ:
			sys_read (f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			sys_write (f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			break;
		case SYS_TELL:
			break;
		case SYS_CLOSE:
			break;
		default:
			printf ("system call exiting\n");
			thread_exit ();
			break;
	}

}

/* power_off()를 호출하며 PintOS를 종료시킨다.
	유저 프로그램에서 OS를 멈출 수 있는 유일한 시스템 콜 이다. */
void
sys_halt (void) {
	power_off (); 
}

/* exit로 호출한 스레드를 종료하는 함수 */
void
sys_exit (int status) {
	printf ("%s: exit(%d)\n", thread_name (), status);
	thread_exit ();
}

/* 해당 fd에 write 하는 함수
	아직 fd 구조를 안만들었지만, 일단 출력만 가능하게 구현. 이후 추가적인 구현 필요 */
int
sys_write (int fd, const void *buffer, unsigned length) {
	check_address (buffer);
	if (fd == 1) {
		putbuf (buffer, length);
		return length;
	}
	else {
		// write 스켈레톤
		return file_write (thread_current ()->fd->fd_address[fd], buffer, length);
	}
}

/* filesys_create를 호출하며 새로운 파일을 만듭니다. */
bool
sys_create (const char *file, unsigned initial_size) {
	check_address (file);
	return filesys_create (file, initial_size);
}

/* 버퍼에 있는 파일의 byte사이즈를 읽고 읽을 수 있는 만큼의 byte를 반환한다.
	읽을 수 없었다면 -1 반환하고, fd 가 0 이라면 input_getc()를 이용하여 키보드 입력을 읽는다. */
int 
sys_read(int fd, void *buffer, unsigned size) {
	
	check_address(buffer);
	int bytes = 0;
	char *next = buffer;

	/* 현재 파일이 비어있거나, */
	struct file *current_file = thread_current() -> fd -> fd_address[fd];

	if (buffer == NULL | current_file == NULL | fd == 1) {
		return -1;
	} 

	/* fd 0 이라면 input_getc()를 이용하여 키보드 입력을 읽는다. */
	if (fd == 0) {
		char user_input;
		for (int i = 0; i < size; i ++) {
			user_input = input_getc();
			*next++ = user_input;

			if (user_input == '\0') {
				break;
			}
		}
	} else { /* 락을 왜 걸어야하지 ? */
		size = file_read(fd, buffer, size);

	}

	return size;
}
