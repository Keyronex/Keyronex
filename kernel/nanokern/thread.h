#ifndef THREAD_H_
#define THREAD_H_

#include <md/intr.h>
#include <md/spl.h>

#include <nanokern/kerndefs.h>
#include <nanokern/queue.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#define kNThreadWaitBlocks 4

typedef struct kspinlock {
	atomic_flag flag;
} kspinlock_t;

typedef struct kdpc {
	/*! Linkage in kcpu_t::dpc_queue */
	TAILQ_ENTRY(kdpc) queue_entry;
	void (*callback)(void *);
	void *arg;
} kdpc_t;

typedef struct kxcallout {
	/*! Linkage in kcpu_t::callout_queue */
	TAILQ_ENTRY(kxcallout) queue_entry;
	/*! DPC to be enqueued on its elapsing */
	kdpc_t dpc;
	/*!
	 * Time (relative to now if head of queue, otherwise to previous
	 * callout) in nanosecs till expiry.
	 */
	nanosec_t nanosecs;
	/*! State of the callout. */
	enum {
		kCalloutDisabled,
		kCalloutPending,
		kCalloutElapsed,
	} state;
} kxcallout_t;

#define KSPINLOCK_INITIALISER    \
	{                        \
		ATOMIC_FLAG_INIT \
	}

typedef enum kwaitstatus {
	/*! the wait condition was met */
	kKernWaitStatusOK,
	/*! the wait timed out */
	kKernWaitStatusTimedOut,
	/*! invalid argument */
	kKernWaitStatusInvalidArgument,
} kwaitstatus_t;

typedef struct kwaitblock {
	/*! link in kdispatchheader_t::waitblock_queue  */
	TAILQ_ENTRY(kwaitblock) queue_entry;

	/*! object being waited on */
	struct kdispatchheader *object;
	/*! thread waiting */
	struct kthread *thread;
	/*! has it been acquired (yet)? */
	bool acquired;
} kwaitblock_t;

typedef enum kdispatchobjecttype {
	kDispatchMutex,
	kDispatchSemaphore,
} kdispatchobjecttype_t;

typedef struct kdispatchheader {
	/*! blocks waiting on this thing */
	TAILQ_HEAD(, kwaitblock) waitblock_queue;

	/*! dispatch object type */
	kdispatchobjecttype_t type : 3;
	/*! signalled status */
	int signalled;
} kdispatchheader_t;

typedef struct ksemaphore {
	kdispatchheader_t hdr;
} ksemaphore_t;

typedef struct kmutex {
	kdispatchheader_t hdr;

	/*! thread currently owning the mutex */
	struct kthread *owner;
} kmutex_t;

typedef enum kthread_state {
	kThreadStateSuspended,
	kThreadStateRunnable,
	kThreadStateRunning,
} kthread_state_t;

typedef struct kthread {
	/*! link in kcpu_t::run_queue, or a wait queue, etc */
	TAILQ_ENTRY(kthread) queue_link;
	/*! link in kprocess_t::thread_list */
	SLIST_ENTRY(kthread) proc_link;

	/*! cpu to which it's bound */
	struct kcpu *cpu;

	/*! kernel stack */
	vaddr_t kstack;

	/*! built-in waitblocks */
	kwaitblock_t integral_waitblocks[kNThreadWaitBlocks];
	/*! waitblocks list being currently waited on */
	kwaitblock_t *waitblocks;
	/*! kind of wait */
	bool iswaitall;
	/* number of objects being waited on */
	size_t nwaits;
	/*! proximate cause of wakeup (only OK or Timeout used) */
	kwaitstatus_t wait_result;
	/*! wait timeout callout */
	kxcallout_t wait_callout;

	/*! (~) process to which it belongs */
	struct kprocess *process;

	kthread_state_t state;
	md_intr_frame_t frame;
} kthread_t;

/*!
 * describes a process
 * (p) = its lock
 */
typedef struct kprocess {
	kspinlock_t lock;

	/* (p) */
	SLIST_HEAD(, kthread) threads;
} kprocess_t;

/*!
 * Describes a CPU core.
 * Locking: s => sched_lock, ~ => invariant
 */
typedef struct kcpu {
	unsigned num;
	md_cpu_t md;

	/* acquired at IPL=dispatch */
	kspinlock_t sched_lock;

	/*! (spldispatch,s) Thread run queue */
	TAILQ_HEAD(, kthread) runqueue;

	/*! (splhigh) DPCs to be executed */
	TAILQ_HEAD(, kdpc) dpc_queue;

	/*! (splhigh) Callouts pending */
	TAILQ_HEAD(, kxcallout) callout_queue;

	/*! (s) currently-running thread */
	kthread_t *running_thread;

	/*! (~) the idle thread for the core */
	kthread_t *idle_thread;

	bool
	    /*! Soft interrupt at dispatch level  */
	    soft_int_dispatch : 1;
} kcpu_t;

/*! Acquire a spinlock without SPL */
static inline void
nk_spinlock_acquire_nospl(kspinlock_t *lock)
{
	while (atomic_flag_test_and_set(&lock->flag)) {
		__asm__("pause");
	}
}

/*! Release a spinlock without SPL */
static inline void
nk_spinlock_release_nospl(kspinlock_t *lock)
{
	atomic_flag_clear(&lock->flag);
}

/*! Initialise a spinlock */
static inline void
nk_spinlock_init(kspinlock_t *lock)
{
	nk_spinlock_release_nospl(lock);
}

/*! Acquire a spinlock at dispatch IPL */
static inline ipl_t
nk_spinlock_acquire(kspinlock_t *lock)
{
	ipl_t ipl = spldispatch();
	nk_spinlock_acquire_nospl(lock);
	return ipl;
}

/*! Acquire a spinlock at custom IPL */
static inline ipl_t
nk_spinlock_acquire_at(kspinlock_t *lock, ipl_t ipl)
{
	ipl_t oldipl = splraise(ipl);
	nk_spinlock_acquire_nospl(lock);
	return oldipl;
}

/*! Release a spinlock and restore saved IPL */
static inline void
nk_spinlock_release(kspinlock_t *lock, ipl_t oldipl)
{
	nk_spinlock_release_nospl(lock);
	splx(oldipl);
}

/*!
 * ONLY TO BE CALLED BY MD (which does so at IPL=high.) Indicates that the per-
 * CPU clock has elapsed.
 */
void nkx_cpu_hardclock(md_intr_frame_t *frame, void *arg);
/*!
 * ONLY TO BE CALLED BY MD (which does so at IPL=dispatch). Indicates that a
 * reschedule IPI was received.
 */
void nkx_reschedule_ipi(md_intr_frame_t *frame, void *arg);
/*!
 * Enqueue a callout
 */
void nkx_callout_enqueue(kxcallout_t *callout);
/*!
 * Dequeue a callout
 */
void nkx_callout_dequeue(kxcallout_t *callout);

/*!
 * Acquire an object
 * \pre nanokernel lock held
 * \pre object is signalled
 */
void nkx_object_acquire(kthread_t *thread, kdispatchheader_t *hdr);

/*!
 * Ran by the signal functions of nanokernel objects when they become signalled.
 * Test whether we should wake a waiting thread and take action accordingly.
 * If its wait is waitAll, then: are all objects signalled?
 *  - if so, acquires them all, and removes the threads' waitblocks from their
 * objects' queues.
 *  - otherwise, don't acquire.
 * If its wait is waitAny, then:
 * - immediately acquires, and removes the thread's waitblocks from their
 * objects' queues.
 * \pre nanokernel lock held
 * @retval false if thread isn't to be woken (and hence waitblock remains in
 * the queue of the caller).
 * @retval true if thread is to be woken (and hence waitblock no longer in queue
 * of the caller).
 */
bool nkx_waiter_maybe_wakeup(kthread_t *thread, kdispatchheader_t *hdr);

/*!
 * Common initialisation for one of the idle threads or a regular thread.
 */
void nkx_thread_common_init(kthread_t *thread, kcpu_t *cpu, kprocess_t *proc);

/*!
 * Enqueue a DPC. (It's run immediately if IPL <= kSPLDispatch)
 */
void nk_dpc_enqueue(kdpc_t *dpc);

/*!
 * Initialise a mutex.
 */
void nk_mutex_init(kmutex_t *mutex);

/*!
 * Release a mutex.
 */
void nk_mutex_release(kmutex_t *mutex);

/*!
 * Initialise a semaphore.
 * \p count Initial count.
 */
void nk_semaphore_init(ksemaphore_t *sem, unsigned count);

/*!
 * Release a semaphore.
 *
 * \p semaphore Pointer to a nanokernel semaphore.
 * \p adjustment Value to add to the semaphore count.
 */
void nk_semaphore_release(ksemaphore_t *sem, unsigned adjustment);

/*!
 * Initialise a thread.
 */
void nk_thread_init(kprocess_t *proc, kthread_t *thread,
    void (*start_fun)(void *), void		*start_arg);

/*!
 * Resume a suspended thread.
 */
void nk_thread_resume(kthread_t *thread);

/*!
 * Wait for a nanokernel object to become signalled.
 *
 * \p objecs pointers to object to wait for.
 * \p reason a constant string describing the purpose of the wait
 * \p isAlertable whether the wait can be interrupted by an alert
 * \p timeout either -1 for no timeout, 0 for instant (called a poll, because it
 * only polls the object) or any other value for a wait time in nanoseconds.
 *
 * @retval kWaitStatusOK object was acquired
 * @retval kKernWaitStatusTimedOut object could not be acquired before
 * timeout/on poll
 */
kwaitstatus_t nk_wait(void *object, const char *reason, bool isUserwait,
    bool isAlertable, nanosec_t timeout);

/*!
 * Wait for several nanokernel objects to become signalled.
 *
 * \p nobjects number of objects to wait for
 * \p objects pointer to array of pointers to objects to wait for.
 * \p reason a constant string describing the purpose of the wait
 * \p isWaitall whether all objects have to be signalled before the wait
 * returns. This is all-or-nothing. If the wait times out, is alerted, or is a
 * poll, then no objects will have been acquired on return. Otherwise
 * \p isAlertable whether the wait can be interrupted by an alert
 * \p timeout either -1 for no timeout, 0 for instant (called a poll, because it
 * only polls the objects) or any other value for a wait time in nanoseconds.
 * \p waitblocks pointer to array of waitblocks to use for the wait, MANDATORY
 * if nobjects > N_THREAD_WAIT_BLOCKS
 *
 * @retval kWaitStatusOK
 * - If \p iswaitall = true, then all objects were acquired
 * - otherwise, at least one was acquired.
 * @retval kKernWaitStatusTimedOut
 *  - If \p iswaitall = true, then at least one of the objects could not be
 * be acquired before timeout/on poll.
 * - otherwise, none were acquired.
 */
kwaitstatus_t nk_wait_multi(size_t nobjects, void *objects[],
    const char *reason, bool isWaitall, bool isUserwait, bool isAlertable,
    nanosec_t timeout, kx_nullable kx_out kwaitblock_t *waitblocks);

/*! Raise a kSPLDispatch-level soft interrupt. */
void nk_raise_dispatch_interrupt(void);

/*! array of all CPUs */
extern kcpu_t **all_cpus;
/*! count of CPUs **/
extern size_t ncpus;

/*! nanokernel structures lock */
extern kspinlock_t nanokern_lock;
/*! the first thread (idle on cpu0) */
extern kthread_t thread0;
/*! the kernel process */
extern kprocess_t proc0;
/*! the bootstrap CPU */
extern kcpu_t cpu0;

#endif /* THREAD_H_ */
