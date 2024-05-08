#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "nanokern/ki.h"
#include "vm/vmp.h"

extern obj_class_t process_class;
extern vm_procstate_t kernel_procstate;
kprocess_t kernel_process = { .vm = &kernel_procstate };

int
ps_thread_create(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg, kprocess_t *ps)
{
	vaddr_t stack;
	kthread_t *thread;

	stack = vm_kalloc(KSTACK_SIZE / PGSIZE, 0);
	if (!stack)
		return -1;

	thread = kmem_alloc(sizeof(*thread));
	if (thread == NULL)
		return -1;

	thread->kstack_base = (void *)stack;
	ki_thread_common_init(thread, NULL, ps, name);
	ke_thread_init_context(thread, fn, arg);

	*out = thread;

	return 0;
}

int
ps_create_kernel_thread(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg)
{
	return ps_thread_create(out, name, fn, arg, &kernel_process);
}

void
ps_exit_this_thread(void)
{
	ke_acquire_scheduler_lock();
	curthread()->state = kThreadStateDone;
	ki_reschedule();
}

int
ps_process_create(kprocess_t **out, bool fork)
{
	int r;
	kprocess_t *proc;

	r = obj_new(&proc, process_class, sizeof(kprocess_t), "a process");
	if (r != 0)
		return r;

	LIST_INIT(&proc->thread_list);

	proc->vm = kmem_alloc(sizeof(vm_procstate_t));
	if (proc->vm == NULL)
		kfatal("handle this\n");

	vm_ps_init(proc);

	*out = proc;

	return 0;
}
