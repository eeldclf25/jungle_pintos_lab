#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/process.h"

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
			sys_open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			break;
		case SYS_READ:
			break;
		case SYS_WRITE:
			f->R.rax = sys_write (f->R.rdi, f->R.rsi, f->R.rdx);
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
	struct thread *current = thread_current ();

	if (current->fd_table->fd_node[fd].type == FD_STDOUT) {
		putbuf (buffer, length);
		return length;
	}
	else if (current->fd_table->fd_node[fd].type == FD_FILE) {
		return file_write (current->fd_table->fd_node[fd].file, buffer, length);
	}
	else {
		// file도 아닌 경우는 뭘 반환해야지?
		sys_exit (-1);
	}
}

/* filesys_create를 호출하며 새로운 파일을 만듭니다. */
bool
sys_create (const char *file, unsigned initial_size) {
	check_address (file);
	return filesys_create (file, initial_size);
}



int
sys_open(const char *name){
	//1. filename이 NULL이거나 사용자 메모리 범위(0x8048000 ~ 0xC0000000) 밖인지 확인 - 유효하지 않으면 exit(-1)
	//예외 처리
	check_address(name);

    //2. filename이 빈 문자열("")인지 확인 - 빈 문자열이면 return -1
	if (name[0] == '\0'){
		return -1;
	}

    //3. filesys_open(filename)으로 파일 열기 - 실패 시 return -1
	struct file *file = filesys_open(name);
	if (file == NULL) {
    	return -1;
	}

   	//4. running_file과 동일하면 file_deny_write 호출
	//포인터 값이 아니라 inode로 비교하는 이유는
	//filesys_open() 같은 함수는 같은 파일을 여러 번 열면 다른 file 포인터를 줘도 같은 inode를 공유
	if (thread_current()->running_file != NULL &&
    file_get_inode(thread_current()->running_file) == file_get_inode(file)) {
    file_deny_write(file);
	}


    //5. fd_table->fd_next에서 fd 할당 (2 <= fd < 63) - 없으면 file_close 후 return -1
    int fd = -1;
	for (int i = thread_current()->fd_table->fd_next; i < FDCOUNT_LIMIT; i++){
		if (thread_current()->fd_table->fd_node[i].type == FD_NONE){
			fd = i;
			break;
		}
	}

	if (fd == -1){
		file_close(file);
		return -1;
	}

	//6. fd_node[fd].type = FD_FILE, fd_node[fd].file = file
	thread_current()->fd_table->fd_node[fd].type = FD_FILE;
	thread_current()->fd_table->fd_node[fd].file = file;
	
	//7. fd_table->fd_next 증가
    thread_current()->fd_table->fd_next = fd + 1;
	
	//8. return fd
	return fd;

}