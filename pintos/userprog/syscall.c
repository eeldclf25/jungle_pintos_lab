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
		exit(-1);
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	switch (f->R.rax)
	{
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit (f->R.rdi);
			break;
		case SYS_FORK:
			break;
		case SYS_EXEC:
			break;
		case SYS_WAIT:
			break;
		case SYS_CREATE:
			create(f-> R.rdi, f -> R.rsi);
			break;
		case SYS_REMOVE:
			break;
		case SYS_OPEN:
			break;
		case SYS_FILESIZE:
			break;
		case SYS_READ:
			break;
		case SYS_WRITE:
			f->R.rax = write (f->R.rdi, f->R.rsi, f->R.rdx);
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
void halt (void) {
	power_off(); 
}

/* exit로 호출한 스레드를 종료하는 함수 */
void
exit (int status) {
	printf("%s: exit(%d)\n", thread_name (), status);
	thread_exit ();
}

/* 해당 fd에 write 하는 함수
	아직 fd 구조를 안만들었지만, 일단 출력만 가능하게 구현. 이후 추가적인 구현 필요 */
int
write (int fd, const void *buffer, unsigned length) {
	// 포인터 체크 필요
	if (fd == 1) {
		putbuf (buffer, length);
		return length;
	}
	else {
		// fd 구현 후 작업
	}
}

bool 
create(const char *file, unsigned initial_size) {
	check_address(file);
	filesys_create(file, initial_size);
}
