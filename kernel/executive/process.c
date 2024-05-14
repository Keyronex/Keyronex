#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/misc.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "nanokern/ki.h"
#include "object.h"
#include "vm/vmp.h"

extern obj_class_t process_class;
extern vm_procstate_t kernel_procstate;

struct {
	struct object_header header;
	eprocess_t process;
} kernel_process_pkg = {
	.process = { .vm = &kernel_procstate }
};
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

	stack = vm_kalloc(KSTACK_SIZE / PGSIZE, 0);
	if (!stack)
		return -1;

	thread = kmem_alloc(sizeof(*thread));
	if (thread == NULL)
		return -1;

	thread->kstack_base = (void *)stack;
	ki_thread_common_init(thread, NULL, &ps->kprocess, name);
	ke_thread_init_context(thread, fn, arg);

	thread->tid = idalloc_alloc(&tid_allocator);

	*out = thread;

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
}

int
ps_process_create(eprocess_t **out, bool fork)
{
	int r;
	eprocess_t *proc;

	r = obj_new(&proc, process_class, sizeof(eprocess_t), "a process");
	if (r != 0)
		return r;

	LIST_INIT(&proc->kprocess.thread_list);

	proc->vm = kmem_alloc(sizeof(vm_procstate_t));
	if (proc->vm == NULL)
		kfatal("handle this\n");

	vm_ps_init(proc);
	for (int i = 0; i < 64; i++)
		proc->handles[i] = NULL;

	*out = proc;

	return 0;
}
