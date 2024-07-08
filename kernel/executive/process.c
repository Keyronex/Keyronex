#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/misc.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "kern/ki.h"
#include "object.h"
#include "vm/vmp.h"

extern obj_class_t process_class, thread_class;
extern vm_procstate_t kernel_procstate;

struct {
	struct object_header header;
	eprocess_t process;
} kernel_process_pkg = { .header = { .class = 2, /* see ex.c */
			     .name = "kernel_process",
			     .refcount = 2,
			     .size = sizeof(eprocess_t) },
	.process = { .vm = &kernel_procstate } };
eprocess_t *kernel_process = &kernel_process_pkg.process;

uint8_t tid_bitmap[UINT16_MAX / 8];
struct id_allocator tid_allocator = STATIC_IDALLOC_INITIALISER(tid_bitmap,
    UINT16_MAX / 8);

int
ps_thread_create(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg, eprocess_t *ps)
{
	vaddr_t stack;
	kthread_t *thread;
	int r;

	stack = vm_kalloc(KSTACK_SIZE / PGSIZE, 0);
	if (!stack)
		return -1;

	r = obj_new(&thread, 3 /* see ex.c */, sizeof(kthread_t), name);
	if (r != 0)
		kfatal("implement thread alloc failure\n");

	thread->kstack_base = (void *)stack;
	ki_thread_common_init(thread, NULL, &ps->kprocess, name);
	ke_thread_init_context(thread, fn, arg);

	thread->tid = idalloc_alloc(&tid_allocator);

	*out = thread;
	obj_retain(ps);

	return 0;
}

int
ps_create_kernel_thread(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg)
{
	return ps_thread_create(out, name, fn, arg, kernel_process);
}

void
ps_exit_this_thread(void)
{
	ke_acquire_scheduler_lock();
	curthread()->state = kThreadStateDone;
	ki_reschedule();
	kfatal("Unreached\n");
}

int
ps_process_create(eprocess_t **out, bool fork)
{
	int r;
	eprocess_t *proc;

	r = obj_new(&proc, process_class, sizeof(eprocess_t), "a process");
	if (r != 0)
		return r;

	ke_process_init(&proc->kprocess);

	proc->vm = kmem_alloc(sizeof(vm_procstate_t));
	if (proc->vm == NULL)
		kfatal("handle this\n");

	vm_ps_init(proc);
	proc->objspace = ex_object_space_create();

	*out = proc;

	return 0;
}

void
ps_early_init(kthread_t *thread0)
{
	ke_process_init(&kernel_process->kprocess);
}

bool
ex_thread_free(void *ptr)
{
	kthread_t *thread = ptr;
	eprocess_t *proc = ex_proc_from_kproc(thread->process);

	vm_kfree((vaddr_t)thread->kstack_base, KSTACK_SIZE / PGSIZE, 0);
	ke_thread_deinit(thread);

	obj_release(proc);

	return true;
}

bool ex_proc_inactive(void *ptr)
{
	eprocess_t *proc = ptr;
}
