#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* process의 fd_table을 초기화 하는 함수
	fd_node를 이중포인터로 사용하여 동적배열로 동작하게 하였으며,
	때문에 fd_node를 처음 calloc으로 포인터 배열을 원하는 인덱스만큼 할당해줘야 함
	이후 fd_node를 생성할때도 할당해준 포인터를 저장해야 함*/
static void
process_fd_init (void) {
	struct thread *current = thread_current ();

	current->fd_table.fd_limit = FD_LIMIT;
	current->fd_table.fd_next = FD_NEXT;
	current->fd_table.fd_node = calloc (current->fd_table.fd_limit, sizeof *current->fd_table.fd_node);
	if (current->fd_table.fd_node == NULL) PANIC("fd table calloc failed");

	current->fd_table.fd_node[0] = malloc (sizeof *current->fd_table.fd_node[0]);
	current->fd_table.fd_node[1] = malloc (sizeof *current->fd_table.fd_node[1]);
	if (current->fd_table.fd_node[0] == NULL || current->fd_table.fd_node[1] == NULL) PANIC("std fd node malloc failed");

	current->fd_table.fd_node[0]->type = FD_STDIN;
	current->fd_table.fd_node[0]->file = NULL;
	current->fd_table.fd_node[1]->type = FD_STDOUT;
	current->fd_table.fd_node[1]->file = NULL;
}

/* process의 fd_table을 초기화 하는 함수
	초기화를 하는데, 매겨변수로 들어온 스레드의 파일 디스크립터로 복사 */
static void
process_fd_duplicate (struct thread *origin) {
	struct thread *current = thread_current ();

	current->fd_table.fd_limit = FD_LIMIT;
	current->fd_table.fd_next = FD_NEXT;
	current->fd_table.fd_node = calloc (current->fd_table.fd_limit, sizeof *current->fd_table.fd_node);
	if (current->fd_table.fd_node == NULL) PANIC("dup fd table calloc failed");

	for (int i = 0; i < origin->fd_table.fd_limit; i++) {
		if (origin->fd_table.fd_node[i] != NULL) {
			current->fd_table.fd_node[i] = malloc (sizeof *current->fd_table.fd_node[i]);
			if (current->fd_table.fd_node[i] == NULL) PANIC("dup std fd node malloc failed");

			current->fd_table.fd_node[i]->type = origin->fd_table.fd_node[i]->type;
			if (origin->fd_table.fd_node[i]->file != NULL)
				current->fd_table.fd_node[i]->file = file_duplicate (origin->fd_table.fd_node[i]->file);
			else
				current->fd_table.fd_node[i]->file = NULL;
		}
	}
}

/* tid_t로 현재 프로세스에서 자식 프로세스가 있는지 찾는 함수
	만약 없을 경우, NULL을 반환 */
static struct child_state*
process_get_child (tid_t child_tid) {
	struct thread *current = thread_current ();

	for (struct list_elem *elem = list_begin(&current->process_child_list); elem != list_end(&current->process_child_list); elem = list_next (elem)) {
		if (child_tid == list_entry(elem, struct child_state, elem)->cheild_tid)
			return list_entry(elem, struct child_state, elem);
	}

	return NULL;
}

/* General process initializer for initd and other process. 
initd 및 기타 프로세스를 위한 일반 프로세스 초기화 함수입니다.*/
static void
process_init (void) {
	struct thread *current = thread_current ();

	process_fd_init ();
}

/* 매개변수로 들어온 프로세스를 복제해서 초기화 하는 함수 */
static void
process_duplicate (struct thread *origin) {
	struct thread *current = thread_current ();

	process_fd_duplicate (origin);
}

/* fd_table에서 비어있는 fd를 가져오는 함수 
	next_fit으로 동작하게 하였으며, fd_table이 꽉 찰 경우 -1 반환 */
static int
process_get_fd (void) {
	struct thread *current = thread_current ();
	int empty_fd = current->fd_table.fd_next;

	do {
		if (current->fd_table.fd_node[empty_fd] == NULL) {
			current->fd_table.fd_next = empty_fd;
			return empty_fd;
		}
		else
			empty_fd = (empty_fd == current->fd_table.fd_limit) ? 0 : ++empty_fd;
	}	while (empty_fd != current->fd_table.fd_next);

	return -1;
}

/* 해당 fd에 들어있는 fd_node를 반환하는 함수
	만약 아무 할당을 안해줬을 경우 NULL을 반환 */
static struct fd_node*
process_check_fd (int check_fd) {
	struct thread *current = thread_current ();
	if (0 <= check_fd && check_fd < current->fd_table.fd_limit) {
		if (current->fd_table.fd_node[check_fd] != NULL) {
			return current->fd_table.fd_node[check_fd];
		}
	}
	return NULL;
}

/* 매개변수로 들어온 문자열로 해당 파일을 오픈하는 함수
	오픈이 가능한 경우, 오픈했던 fd 인덱스를 반환, 안되면 -1 반환 */
int
process_file_open (const char *file_name) {
	struct thread *current = thread_current ();
	struct file *open_file;
	int return_fd;

	if ((return_fd = process_get_fd ()) != -1 && (open_file = filesys_open (file_name)) != NULL) {
		current->fd_table.fd_node[return_fd] = malloc (sizeof *current->fd_table.fd_node[return_fd]);
		if (current->fd_table.fd_node[return_fd] == NULL) PANIC("file open malloc failed");
		current->fd_table.fd_node[return_fd]->file = open_file;
		current->fd_table.fd_node[return_fd]->type = FD_FILE;
		return return_fd;
	}
	return -1;
}

/* 매개변수로 들어온 fd로 해당 파일을 size를 반환하는 함수
	오픈이 가능한 경우, 오픈했던 fd 인덱스를 반환, 안되면 -1 반환 */
int
process_file_length (int fd) {
	struct fd_node *node;

	if ((node = process_check_fd (fd)) && node->type == FD_FILE) {
		return file_length (node->file);
	}
	return -1;
}

/* 매개변수로 들어온 fd로 해당 파일을 size 만큼 읽는 함수
	가능한 읽은 만큼 buffer에 저장하고 읽은 size를 반환, 안되면 -1 반환 */
int
process_file_read (int fd, void *buffer, unsigned size) {
	struct fd_node *node;

	if (node = process_check_fd (fd)) {
		if (node->type == FD_FILE)
			return file_read (node->file, buffer, size);
		else if (node->type == FD_STDIN)
			return input_getc ();
	}
	return -1;
}

/* 매개변수로 들어온 fd로 해당 파일을 size 만큼 작성하는 함수
	buffer에 있는 문자열을 가능한 만큼 작성하고 작성한 size를 반환, 안되면 -1 반환 */
int
process_file_write (int fd, const void *buffer, unsigned size) {
	struct fd_node *node;

	if (node = process_check_fd (fd)) {
		if (node->type == FD_FILE)
			return file_write (node->file, buffer, size);
		else if (node->type == FD_STDOUT) {
			putbuf (buffer, size);
			return size;
		}
	}
	return -1;
}

/* 현재 프로세스의 파일디스크립터에 해당 fd의 pos를 업데이트 하는 함수 */
void
process_file_seek (int fd, unsigned position) {
	struct fd_node *node;

	if (node = process_check_fd (fd))
		file_seek (node->file, position);
}

/* 열려진 파일 fd에서 읽히거나 써질 다음 바이트의 위치를 반환 */
unsigned
process_file_tell (int fd) {
	struct fd_node *node;

	if (node = process_check_fd (fd))
		return file_tell (node->file);
}

/* 매개변수로 들어온 fd에 파일이 있다면 close하는 함수 */
void
process_file_close (int fd) {
	struct fd_node *node;

	if (node = process_check_fd (fd)) {
		thread_current ()->fd_table.fd_node[fd] = NULL;
		file_close (node->file);
		free (node);
	}
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
/* FILE_NAME에서 "initd"라는 첫 번째 사용자 프로그램을 시작합니다.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄링(또는 종료)될 수 있습니다.
 * initd의 스레드 id를 반환하며, 스레드를 생성할 수 없으면 TID_ERROR를 반환합니다.
 * 반드시 한 번만 호출해야 합니다. */
tid_t
process_create_initd (const char *file_name) {
	char fname_buf[16];
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 첫번째 인자가 파일 이름이니까 이것만 복사 */
	strlcpy(fname_buf, file_name, (strcspn(file_name, " ") + 1));

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (fname_buf, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
/* 첫 번째 사용자 프로세스를 실행하는 스레드 함수입니다. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
/* 현재 프로세스를 `name`으로 복제합니다. 새 프로세스의 스레드 id를 반환하며,
 * 스레드를 생성할 수 없으면 TID_ERROR를 반환합니다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	struct thread *cur = thread_current ();
	struct child_state *child;
	tid_t child_tid;

	memcpy (&cur->fork_tf, if_, sizeof (struct intr_frame));
	if ((child_tid = thread_create (name, PRI_DEFAULT, __do_fork, cur)) != TID_ERROR)
		sema_down (&process_get_child (child_tid)->cheild_ptr->fork_sema);
	else
		return TID_ERROR;

	return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
/* 이 함수를 pml4_for_each에 전달하여 부모의 주소 공간을 복제합니다.
 * 이 코드는 project 2에서만 사용됩니다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 1. TODO: parent_page가 커널 페이지라면 즉시 반환합니다. */
	if (is_kern_pte(pte)) return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	/* 2. 부모의 pml4에서 VA를 해석합니다. */
	if ((parent_page = pml4_get_page (parent->pml4, va)) == NULL) return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	/* 3. TODO: 자식 프로세스를 위해 PAL_USER 페이지를 새로 할당하고, 결과를 NEWPAGE에 저장합니다. */
	if ((newpage = palloc_get_page (PAL_USER | PAL_ZERO)) == NULL) return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	/* 4. TODO: 부모의 페이지를 새 페이지에 복제하고, 부모의 페이지가 쓰기 가능한지 확인하여 WRITABLE 값을 설정합니다. */
	memcpy (newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	/* 5. WRITABLE 권한으로 자식의 페이지 테이블에 VA 주소에 새 페이지를 추가합니다. */
	/* 6. TODO: if fail to insert page, do error handling. */
	/* 6. TODO: 페이지 삽입에 실패하면 에러 처리를 합니다. */
	if (pml4_set_page (current->pml4, va, newpage, writable) == false) return false;

	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
/* 부모의 실행 컨텍스트를 복사하는 스레드 함수입니다.
 * 힌트) parent->tf는 프로세스의 사용자 영역 컨텍스트를 가지고 있지 않습니다.
 *       즉, process_fork의 두 번째 인자를 이 함수에 전달해야 합니다. */

static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	struct intr_frame *parent_if = &parent->fork_tf;
	bool succ = true;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* fork니까 부모의 스레드를 복사 */
	current->current_file = file_duplicate (parent->current_file);
	file_deny_write (current->current_file);
	process_duplicate (parent);

	/* Finally, switch to the newly created process. */
	sema_up (&current->fork_sema);
	if (succ)
		do_iret (&if_);
error:
	thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name;
	bool success;

	file_name = palloc_get_page(0);
	if (file_name == NULL)
		return -1;
	strlcpy (file_name, f_name, PGSIZE);
	if (is_kernel_vaddr (f_name))
		palloc_free_page (f_name);

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. 
 * 
 * 스레드 TID가 종료될 때까지 기다렸다가, 그 종료 상태(exit status)를 반환합니다.
만약 그 스레드가 커널에 의해 종료되었으면(예: 예외 때문에 강제 종료된 경우), -1을 반환합니다.
TID가 유효하지 않거나, 호출한 프로세스의 자식이 아니거나, 
해당 TID에 대해 process_wait()가 이미 성공적으로 호출된 적이 있다면, 
기다리지 않고 즉시 -1을 반환합니다.
이 함수는 문제 2-2에서 구현될 예정입니다.
현재는 아무 동작도 하지 않습니다.*/
int
process_wait (tid_t child_tid UNUSED) {
	struct thread *current = thread_current ();

	for (struct list_elem *elem = list_begin(&current->process_child_list); elem != list_end(&current->process_child_list); elem = list_next (elem)) {
		struct child_state *child_elem = list_entry(elem, struct child_state, elem);

		if (child_tid == child_elem->cheild_tid) {
			if (child_elem->is_dying == false) {
				sema_down (&child_elem->cheild_ptr->process_current_state_sema);
			}

			int exit_state = child_elem->exit_state;
			list_remove (elem);
			free (child_elem);
			return exit_state;
		}
	}

	return -1;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	struct fd_node *file_ptr;

	/* 부모 프로세스에서 현재 프로세스의 list를 찾고 값을 업데이트 하고 exit */
	for (struct list_elem *elem = list_begin(&curr->process_parent->process_child_list); elem != list_end(&curr->process_parent->process_child_list); elem = list_next (elem)) {
		struct child_state *child_elem = list_entry(elem, struct child_state, elem);

		if (child_elem->cheild_ptr == curr) {
			child_elem->is_dying = true;
			child_elem->exit_state = curr->exit_status;
		}
	}

	/* 현재 프로세스가 갖고있는 fd_table을 모두 닫고 할장 해제 */
	for (int i = 0; i < curr->fd_table.fd_limit; i++) {
		if (file_ptr = process_check_fd (i)) {
			process_file_close (i);
		}
	}
	free (curr->fd_table.fd_node);

	/* 프로세스 자체가 열고있는 파일 close */
	file_close (curr->current_file);

	/* exit 하면서 부모 스레드가 이 스레드가 끝날때까지 대기하기 위해 sema_down을 할 경우, sema_up을 실행 */
	sema_up (&curr->process_current_state_sema);

	process_cleanup ();	
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	char fname_buf[16];
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	
	/* 인자 첫번째의 실행 파일 이름만 복사 */
	strlcpy(fname_buf, file_name, (strcspn(file_name, " ") + 1));

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (fname_buf);
	if (file == NULL) {
		printf ("load: %s: open failed\n", fname_buf);
		goto done;
	}

	if (t->current_file == NULL) {
		t->current_file = file;
		file_deny_write (file);
	}
	else {
		file_allow_write (t->current_file);
		t->current_file = file;
		file_deny_write (file);
	}

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", fname_buf);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* 들어온 file_name 파싱 */
	char *token, *save, *cmd_str[64];
	int count = 0;
	token = strtok_r(file_name, " ", &save);
	do {
		cmd_str[count++] = token;
		token = strtok_r(NULL, " ", &save);
	} while (token != NULL);

	/* 스택의 rsp부터 차근차근 반대로 삽입 */
	char *cmd_ptr[64];
	for (int i = count - 1; i >= 0; --i) {
		if_->rsp = ((char*)if_->rsp - (strlen(cmd_str[i]) + 1));
		memcpy(if_->rsp, cmd_str[i], (strlen(cmd_str[i]) + 1));
		cmd_ptr[i] = if_->rsp;
	}

	/* 인자 문자열을 넣은 후 패딩 */
	uintptr_t padding = if_->rsp % 8;
	if_->rsp = ((char*)if_->rsp - (padding + 8));	// 패딩 뿐만 아니라 문자열 주소의 끝을 나타내는 NULL을 위해서 8 추가
	memset (if_->rsp, 0, (padding + 8));

	/* 인자 문자열의 주소를 삽입 */
	for (int i = count - 1; i >= 0; --i) {
		if_->rsp = ((char*)if_->rsp - (8));
		memcpy(if_->rsp, &cmd_ptr[i], (8));
	}

	/* 가짜 return address 삽입 */
	if_->rsp = ((char*)if_->rsp - (8));
	memset (if_->rsp, 0, (8));

	/* rdi, rsi 레지스터 삽입 */
	if_->R.rdi = count;
	if_->R.rsi = (char *)if_->rsp + (8);

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	스택의 바닥(stack_bottom)에 스택을 매핑하고, 즉시 해당 페이지를 할당(claim)하세요.
	 * TODO: If success, set the rsp accordingly.
	 성공했다면, rsp 레지스터를 그에 맞게 설정하세요
	 * TODO: You should mark the page is stack. 
	 해당 페이지를 스택으로 표시*/
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */
