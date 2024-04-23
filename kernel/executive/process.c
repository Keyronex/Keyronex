#include "kdk/kmem.h"
#include "kdk/vm.h"
#include "nanokern/ki.h"

extern vm_procstate_t kernel_procstate;
kprocess_t kernel_process = { .vm = &kernel_procstate };

int
ps_create_kernel_thread(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg)
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
	ki_thread_common_init(thread, NULL, NULL, name);
	ke_thread_init_context(thread, fn, arg);

	*out = thread;

	return 0;
}

void
ps_exit_this_thread(void)
{
	ke_acquire_scheduler_lock();
	curthread()->state = kThreadStateDone;
	ki_reschedule();
}
