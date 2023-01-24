#include "arch/sys_arch.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nanokern/thread.h"
#include <kern/kmem.h>

u32_t sys_now(void) {
	return cpu0.ticks / (NS_PER_S / 1000);
}

err_t sys_mutex_new(sys_mutex_t *mutex){
	nk_mutex_init(mutex);
	return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
	nk_wait(mutex, "lwip_sys_mutex_lock", false, false, -1);
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
	nk_mutex_release(mutex);
}

void sys_mutex_free(sys_mutex_t *mutex ) {
	kassert(!mutex->hdr.signalled);
}

/* sys_mutex_valid unused? */
/* sys_mutex_set_invalid unused? */

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
	nk_semaphore_init(sem, count);
	return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem) {
	nk_semaphore_release(sem, 1);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
	kwaitstatus_t r = nk_wait(sem, "lwip_sys_sem_wait", false, false, timeout *  (NS_PER_S / 1000));
	if (r == kKernWaitStatusOK)
		return ERR_OK;
	else {
		kassert(r == kKernWaitStatusTimedOut);
		return SYS_ARCH_TIMEOUT;
	}
}

void sys_sem_free(sys_sem_t *sem) {

}

int sys_sem_valid(sys_sem_t *sem) {
	return sem->hdr.type = kDispatchSemaphore;
}

void sys_sem_set_invalid(sys_sem_t *sem) {
	sem->hdr.type = -1;
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
	nk_msgqueue_init(mbox, size);
	return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
	nk_msgq_post(mbox, msg);
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
	nk_msgq_post(mbox, msg);
	return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
	nk_msgq_post(mbox, msg);
	return ERR_OK;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout){
	uint64_t nanosecs  = timeout == 0 ? -1 : timeout * (NS_PER_S/1000);
	kwaitstatus_t r;

	r = nk_wait(mbox, "lwip_mbox_fetch", false, false, nanosecs);
	if (r == kKernWaitStatusTimedOut) {
		return SYS_ARCH_TIMEOUT;
	}

	r = nk_msgq_read(mbox, msg);
	kassert(r == 0);

	return 0;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
	sys_arch_mbox_fetch(mbox, msg, 0);
	return 0;
}

void sys_mbox_free(sys_mbox_t *mbox) {
	/* todo */
}

int sys_mbox_valid(sys_mbox_t *mbox){
	return mbox->hdr.type == kDispatchMsgQueue ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
	mbox->hdr.type = -1;
}

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn thread, void *arg, int stacksize, int prio)
{
	kprintf("LWIP requesting creation of thread named %s\n", name);
	kthread_t *thr = kmem_alloc(sizeof(*thread));
	nk_thread_init(&proc0, thr, thread, arg);
	nk_thread_resume(thr);
	return thr;
}


void sys_init(void) {
	kprintf("LWIP sys_init callback\n");
}
