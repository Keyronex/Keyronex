#ifndef ARCH_SYS_ARCH_H
#define ARCH_SYS_ARCH_H

#include <stdbool.h>
#include <stdint.h>

#include "kdk/nanokern.h"
#include "kdk/executive.h"

#ifdef __cplusplus
extern "C" {
#endif

/* is this right? */
#define SYS_ARCH_DECL_PROTECT(x) static ipl_t ipl_##x

extern kthread_t *lwprot_thread;
extern size_t lwprot_nest;

/* implements a recursive spinlock */
static inline ipl_t
sys_arch_protect(void)
{
	kthread_t *thread = curthread();
	ipl_t ipl = spldpc();

	if (__atomic_load_n(&lwprot_thread, __ATOMIC_ACQUIRE) != thread) {
		kthread_t *expected = NULL;
		while (!__atomic_compare_exchange_n(&lwprot_thread, &expected,
		    thread, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			expected = NULL;
	}
	__atomic_fetch_add(&lwprot_nest, 1, __ATOMIC_ACQUIRE);

	return ipl;
}

static inline void
sys_arch_unprotect(ipl_t ipl)
{
	if (__atomic_fetch_sub(&lwprot_nest, 1, __ATOMIC_RELEASE) == 1)
		__atomic_store_n(&lwprot_thread, 0, __ATOMIC_RELEASE);
	splx(ipl);
}

/* special interfaces */
void ksp_reset_timer(uint32_t abs);

#define SYS_ARCH_PROTECT(x) ipl_##x = sys_arch_protect()
#define SYS_ARCH_UNPROTECT(x) sys_arch_unprotect(ipl_##x)

extern kspinlock_t tcpip_lock;
typedef ksemaphore_t sys_sem_t;
typedef kmutex_t sys_mutex_t;
typedef kmsgqueue_t sys_mbox_t;
typedef kthread_t *sys_thread_t;

/* todo lmao */
#define LWIP_RAND() (0xdeadbeef)

#ifdef __cplusplus
}
#endif

#endif /* ARCH_SYS_ARCH_H */
