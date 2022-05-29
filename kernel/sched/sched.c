#include <sched/sched.h>
#include <int/apic.h>
#include <vector.h>
#include <cpu.h>
#include <mm/pmm.h>
#include <string.h>
#include <debug.h>
#include <elf.h>
#include <mm/mmap.h>
#include <types.h>

static struct hash_table task_list;

struct bitmap pid_bitmap = {
	.data = NULL,
	.size = 0,
	.resizable = true
};

char sched_lock;

// does not lock **remember** 
struct sched_task *sched_translate_pid(pid_t pid) {
	return hash_table_search(&task_list, &pid, sizeof(pid));
}

// does not lock **remember** 
struct sched_thread *sched_translate_tid(pid_t pid, tid_t tid) {
	struct sched_task *task = sched_translate_pid(pid);
	if(task == NULL) {
		return NULL;
	}

	return hash_table_search(&task->thread_list, &tid, sizeof(tid));
}

struct sched_thread *find_next_thread(struct sched_task *task) {
	struct sched_thread *ret = NULL;

	for(size_t i = 0, cnt = 0; i < task->thread_list.capacity; i++) {
		if(task->thread_list.data[i] == NULL) {
			continue;
		}

		struct sched_thread *next_thread = task->thread_list.data[i];
		next_thread->idle_cnt++;

		if(next_thread->status == TASK_WAITING && cnt < next_thread->idle_cnt) {
			cnt = next_thread->idle_cnt;
			ret = next_thread;
		}
	}

	return ret;
}

struct sched_task *find_next_task() {
	struct sched_task *ret = NULL;

	for(size_t i = 0, cnt = 0; i < task_list.capacity; i++) {
		if(task_list.data[i] == NULL) {
			continue;
		}

		struct sched_task *next_task = task_list.data[i];
		next_task->idle_cnt++;

		if(next_task->status == TASK_WAITING && cnt < next_task->idle_cnt) {
			cnt = next_task->idle_cnt;
			ret = next_task;
		}
	}

	return ret;
}

#define EXIT_RESCHEDULE() ({ \
	if(regs->cs & 0x3) { \
		swapgs(); \
	} \
	xapic_write(XAPIC_EOI_OFF, 0); \
	spinrelease(&sched_lock); \
	return; \
})

void reschedule(struct registers *regs, void*) {
	if(__atomic_test_and_set(&sched_lock, __ATOMIC_ACQUIRE)) {
		return;
	}

	if(regs->cs & 0x3) {
		swapgs();
	}

	struct sched_task *next_task = find_next_task();
	if(next_task == NULL) {
		EXIT_RESCHEDULE();
	}

	struct sched_thread *next_thread = find_next_thread(next_task);
	if(next_thread == NULL) {
		EXIT_RESCHEDULE();
	}

	if(CORE_LOCAL->tid != -1 && CORE_LOCAL->pid != -1) {
		struct sched_task *last_task = sched_translate_pid(CORE_LOCAL->pid);		 
		if(last_task == NULL) {
			EXIT_RESCHEDULE();
		}

		struct sched_thread *last_thread = sched_translate_tid(CORE_LOCAL->pid, CORE_LOCAL->tid);
		if(last_thread == NULL) {
			EXIT_RESCHEDULE();
		}

		last_thread->status = TASK_WAITING;
		last_task->status = TASK_WAITING;

		last_thread->errno = CORE_LOCAL->errno;
		last_thread->regs = *regs;
		last_thread->user_fs_base = get_user_fs();
		last_thread->user_gs_base = get_user_gs();
		last_thread->user_stack = CORE_LOCAL->user_stack;
	}

	CORE_LOCAL->pid = next_task->pid;
	CORE_LOCAL->tid = next_thread->tid;
	CORE_LOCAL->errno = next_thread->errno;
	CORE_LOCAL->kernel_stack = next_thread->kernel_stack;
	CORE_LOCAL->user_stack = next_thread->user_stack;

	CORE_LOCAL->page_table = next_task->page_table;

	vmm_init_page_table(CORE_LOCAL->page_table);

	next_thread->idle_cnt = 0;
	next_task->idle_cnt = 0;
	next_task->status = TASK_RUNNING;
	next_thread->status = TASK_RUNNING;

	set_user_fs(next_thread->user_fs_base);
	set_user_gs(next_thread->user_gs_base);

	if(next_thread->regs.cs & 0x3) {
		swapgs();
	}

	xapic_write(XAPIC_EOI_OFF, 0);
	spinrelease(&sched_lock);

	asm volatile (
		"mov %0, %%rsp\n\t"
		"pop %%r15\n\t"
		"pop %%r14\n\t"
		"pop %%r13\n\t"
		"pop %%r12\n\t"
		"pop %%r11\n\t"
		"pop %%r10\n\t"
		"pop %%r9\n\t"
		"pop %%r8\n\t"
		"pop %%rsi\n\t"
		"pop %%rdi\n\t"
		"pop %%rbp\n\t"
		"pop %%rdx\n\t"
		"pop %%rcx\n\t"
		"pop %%rbx\n\t"
		"pop %%rax\n\t"
		"addq $16, %%rsp\n\t"
		"iretq\n\t"
		:: "r" (&next_thread->regs)
	);
}

struct sched_task *sched_default_task() {
	struct sched_task *task = alloc(sizeof(struct sched_task));

	task->pid = bitmap_alloc(&pid_bitmap);
	task->status = TASK_YIELD;
	task->fd_bitmap.resizable = true;

	bitmap_alloc(&task->fd_bitmap); // STDIN
	bitmap_alloc(&task->fd_bitmap); // STDOUT
	bitmap_alloc(&task->fd_bitmap); // STDERR

	task->tid_bitmap = (struct bitmap) {
		.data = NULL,
		.size = 0,
		.resizable = true
	};

	if(CURRENT_TASK != NULL) {
		task->ppid = CURRENT_TASK->pid;
	} else {
		task->ppid = -1;
	}

	hash_table_push(&task_list, &task->pid, task, sizeof(task->pid));

	return task;
}

struct sched_thread *sched_default_thread(struct sched_task *task) {
	struct sched_thread *thread = alloc(sizeof(struct sched_thread));

	thread->pid = task->pid;
	thread->tid = bitmap_alloc(&task->tid_bitmap);
	thread->status = TASK_YIELD;

	thread->kernel_stack = pmm_alloc(DIV_ROUNDUP(THREAD_KERNEL_STACK_SIZE, PAGE_SIZE), 1) + HIGH_VMA;

	hash_table_push(&task->thread_list, &thread->tid, thread, sizeof(thread->tid));

	return thread;
}

static uint64_t sched_arg_placement(struct sched_arguments *arguments, uint64_t *ptr, struct aux *aux) {
	uint64_t rsp = (uint64_t)ptr;

	for(size_t i = 0; i < arguments->envp_cnt; i++) {
		char *element = arguments->envp[i];
		ptr = (uint64_t*)((void*)ptr - (strlen(element) + 1));
		strcpy((void*)ptr, element);
	}

	for(size_t i = 0; i < arguments->argv_cnt; i++) {
		char *element = arguments->argv[i];
		ptr = (uint64_t*)((void*)ptr - (strlen(element) + 1));
		strcpy((void*)ptr, element);
	}

	ptr = (uint64_t*)((uintptr_t)ptr - ((uintptr_t)ptr & 0xf)); // align 16

	if((arguments->argv_cnt + arguments->envp_cnt + 1) & 1) {
		ptr--;
	}

	ptr -= 10;

	ptr[0] = ELF_AT_PHNUM; ptr[1] = aux->at_phnum;
	ptr[2] = ELF_AT_PHENT; ptr[3] = aux->at_phent;
	ptr[4] = ELF_AT_PHDR;  ptr[5] = aux->at_phdr;
	ptr[6] = ELF_AT_ENTRY; ptr[7] = aux->at_entry;
	ptr[8] = 0; ptr[9] = 0;

	*(--ptr) = 0;
	ptr -= arguments->envp_cnt;

	for(size_t i = 0; i < arguments->envp_cnt; i++) {
		rsp -= strlen(arguments->envp[i]) + 1;
		ptr[i] = rsp;
	}

	*(--ptr) = 0;
	ptr -= arguments->argv_cnt;

	for(size_t i = 0; i < arguments->argv_cnt; i++) {
		rsp -= strlen(arguments->argv[i]) + 1;
		ptr[i] = rsp;
	}

	*(--ptr) = arguments->argv_cnt;

	return (uint64_t)ptr;
}

struct sched_thread *sched_thread_exec(struct sched_task *task, uint64_t rip, uint16_t cs, struct aux *aux, struct sched_arguments *arguments) {
	struct sched_thread *thread = sched_default_thread(task);

	thread->regs.rip = rip;
	thread->regs.cs = cs;
	thread->regs.rflags = 0x202;

	thread->user_gs_base = 0;
	thread->user_fs_base = 0;

	if(cs & 0x3) {
		thread->regs.ss = cs - 8;
		thread->user_stack = (uint64_t)mmap(	task->page_table,
												NULL,
												THREAD_USER_STACK_SIZE,
												MMAP_PROT_READ | MMAP_PROT_WRITE | MMAP_PROT_USER,
												MMAP_MAP_ANONYMOUS,
												0,
												0
										   ) + THREAD_USER_STACK_SIZE;
		thread->regs.rsp = sched_arg_placement(arguments, (void*)thread->user_stack, aux);
	} else {
		thread->regs.ss = cs + 8;
		thread->regs.rsp = thread->kernel_stack;
	}

	return thread;
}

ssize_t tty_read(struct asset *, void*, off_t, off_t cnt, void *buffer);
ssize_t tty_write(struct asset *, void*, off_t, off_t cnt, const void *buffer);

struct sched_task *sched_task_exec(const char *path, uint16_t cs, struct sched_arguments *arguments) {
	spinlock(&sched_lock);

	struct sched_task *task = sched_default_task();

	task->page_table = alloc(sizeof(struct page_table));
	vmm_default_table(task->page_table);

	vmm_init_page_table(task->page_table);

	struct sched_task *current_task = CURRENT_TASK;
	CORE_LOCAL->pid = task->pid;

	int fd = fd_open(path, 0);
	if(fd == -1) {
		spinrelease(&sched_lock); 
		return NULL;
	}

	char *ld_path = NULL;

	struct aux aux;
	if(elf_load(task->page_table, &aux, fd, 0, &ld_path) == -1) { 
		spinrelease(&sched_lock); 
		return NULL;
	}

	uint64_t entry_point = aux.at_entry;

	if(ld_path) {
		int ld_fd = fd_open(ld_path, 0);
		if(ld_fd == -1) {
			spinrelease(&sched_lock); 
			return NULL;
		}

		struct aux ld_aux;
		if(elf_load(task->page_table, &ld_aux, ld_fd, 0x40000000, NULL) == -1) { 
			spinrelease(&sched_lock); 
			return NULL;
		}

		entry_point = ld_aux.at_entry;
	}

	struct fd_handle *stdin_handle = alloc(sizeof(struct fd_handle));

	*stdin_handle = (struct fd_handle) { 
		.asset = alloc(sizeof(struct asset)),
		.fd_number = 0,
		.flags = O_RDONLY,
		.position = 0
	};

	stdin_handle->asset->stat = alloc(sizeof(struct stat));
	stdin_handle->asset->stat->st_mode = S_IRUSR | S_IWUSR;
	stdin_handle->asset->read = tty_read;

	struct fd_handle *stdout_handle = alloc(sizeof(struct fd_handle));

	*stdout_handle = (struct fd_handle) { 
		.asset = alloc(sizeof(struct asset)),
		.fd_number = 1,
		.flags = O_WRONLY,
		.position = 0
	};

	stdout_handle->asset->stat = alloc(sizeof(struct stat));
	stdout_handle->asset->stat->st_mode = S_IWUSR | S_IRUSR;
	stdout_handle->asset->write = tty_write;

	struct fd_handle *stderr_handle = alloc(sizeof(struct fd_handle));

	*stderr_handle = (struct fd_handle) { 
		.asset = alloc(sizeof(struct asset)),
		.fd_number = 2,
		.flags = O_WRONLY,
		.position = 0
	};

	stderr_handle->asset->stat = alloc(sizeof(struct stat));
	stderr_handle->asset->stat->st_mode = S_IWUSR | S_IRUSR;
	stderr_handle->asset->write = tty_write;

	hash_table_push(&task->fd_list, &stdin_handle->fd_number, stdin_handle, sizeof(stdin_handle->fd_number));
	hash_table_push(&task->fd_list, &stdout_handle->fd_number, stdout_handle, sizeof(stdout_handle->fd_number));
	hash_table_push(&task->fd_list, &stderr_handle->fd_number, stderr_handle, sizeof(stderr_handle->fd_number));

	struct sched_thread *thread = sched_thread_exec(task, entry_point, cs, &aux, arguments);

	if(thread == NULL) {
		spinrelease(&sched_lock);
		return NULL;
	}

	CORE_LOCAL->pid = current_task->pid;
	vmm_init_page_table(current_task->page_table);

	spinrelease(&sched_lock);

	task->status = TASK_WAITING;
	thread->status = TASK_WAITING;

	return task;
}

void syscall_getpid(struct registers *regs) {
	regs->rax = CORE_LOCAL->pid;
}

void syscall_getppid(struct registers *regs) {
	regs->rax = CURRENT_TASK->ppid;
}

void syscall_gettid(struct registers *regs) {
	regs->rax = CORE_LOCAL->tid;
}
