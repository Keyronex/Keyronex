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
	/*! Is it bound to a queue, ready to be run? */
	bool bound;
} kdpc_t;

typedef struct kxcallout {
	/*! Linkage in kcpu_t::callout_queue */
	TAILQ_ENTRY(kxcallout) queue_entry;
	/*! Name for debugging */
	const char *name;
	/*! DPC to be enqueued on its elapsing */
	kdpc_t dpc;
	/*!
	 * Time (in terms of nanosecs) till expiry.
	 */
	nanosec_t nanosecs;
	/*!
	 * Absolute time (in terms of cpu->ticks) of expiry.
	 */
	nanosec_t deadline;
	/*! State of the callout. */
	enum {
		kCalloutDisabled,
		kCalloutPending,
		kCalloutElapsed,
	} state;
	/*! If kCalloutPending/kCalloutElapsed, on which CPU? */
	struct kcpu *cpu;
} kxcallout_t;

TAILQ_HEAD(kxcallout_queue, kxcallout);

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
	/*! internal status, wait is currently underway */
	kKernWaitStatusWaiting,
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
	kDispatchTimer,
	kDispatchMsgQueue,
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

#define KMUTEX_INITIALIZER(kmutex)                              \
	{                                                       \
		.hdr.type = kDispatchMutex, .hdr.signalled = 1, \
		.hdr.waitblock_queue = TAILQ_HEAD_INITIALIZER(  \
		    (kmutex).hdr.waitblock_queue)               \
	}

typedef struct ktimer {
	kdispatchheader_t hdr;

	/*! callout associated */
	kxcallout_t callout;
} ktimer_t;

typedef struct kmsgqueue {
	kdispatchheader_t hdr;

	/* size (must be power of 2) */
	size_t size;
	/* write head */
	size_t writehead;
	/* read head */
	size_t readhead;

	/*! message enqueuing semaphore */
	ksemaphore_t sem;

	/*! message ringbuf */
	void **messages;
} kmsgqueue_t;

typedef enum kthread_state {
	kThreadStateSuspended,
	kThreadStateRunnable,
	kThreadStateRunning,
	kThreadStateWaiting,
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

	/* temporary for the sake of lwip */
#undef errno
	int errno;

	/*! (~) process to which it belongs */
	struct kprocess *process;

	/*! current thread state */
	kthread_state_t state;

	/*! thread saved frame */
	md_intr_frame_t frame;
	/*! saved IPL */
	ipl_t saved_ipl;

	/*! remaining timeslice in ticks */
	uint64_t timeslice;
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
 * Locking:
 * - a => atomic
 * - s => kcpu::sched_lock
 * - c => ::callouts_lock
 * - ~ => invariant
 */
typedef struct kcpu {
	unsigned num;
	md_cpu_t md;

	/* acquired at IPL=dispatch */
	kspinlock_t sched_lock;

	/* (a) ticks (actually nanosecs) */
	_Atomic nanosec_t ticks;

	/*! (spldispatch,s) Thread run queue */
	TAILQ_HEAD(, kthread) runqueue;

	/*! (splhigh) DPCs to be executed */
	TAILQ_HEAD(, kdpc) dpc_queue;

	/*! (splhigh,c) Callouts pending */
	struct kxcallout_queue callout_queue;

	/*! (s) currently-running thread */
	kthread_t *running_thread;

	/*! (~) the idle thread for the core */
	kthread_t *idle_thread;

	/*! (?) preemption dpc */
	kdpc_t preempt_dpc;

	bool
	    /*! Soft interrupt at dispatch level  */
	    soft_int_dispatch : 1,
	    /*! Entering scheduler soon. */
	    entering_scheduler : 1;
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
 * Preemption DPC callback
 */
void nkx_preempt_dpc(void *arg);
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
 * Enqueue a callout on the local CPU.
 */
void nkx_callout_enqueue(kxcallout_t *callout);
/*!
 * Dequeue a callout from its CPU.
 * \pre callout is pending
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
void nkx_thread_common_init(kthread_t *thread, kcpu_t *cpu, kprocess_t *proc,
    const char *name);

/*!
 * Enter the scheduler (with old IPL provided)
 */
void nkx_do_reschedule(ipl_t ipl);

/*!
 * Enqueue a DPC. (It's run immediately if IPL <= kSPLDispatch)
 */
void nk_dpc_enqueue(kdpc_t *dpc);

/*!
 * Initialise a nanokernel messagequeue with the given capacity.
 */
void nk_msgqueue_init(kmsgqueue_t *msgq, unsigned count);

/*!
 * Post a message to a nanokernel messagequeue. Waits until it can be done.
 */
void nk_msgq_post(kmsgqueue_t *queue, void *msg);

/*!
 * Read a message from a nanokernel messagequeue.
 * @retval 0 a message was retrieved
 * @retval 1 no messages were pending
 */
int nk_msgq_read(kmsgqueue_t *queue, void **msg);

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
 * Initialise a timer.
 */
void nk_timer_init(ktimer_t *timer);

/*!
 * Set a timer for a given time.
 */
void nk_timer_set(ktimer_t *timer, uint64_t nanosecs);

/*!
 * Initialise a thread.
 */
void nk_thread_init(kprocess_t *proc, kthread_t *thread,
    void (*start_fun)(void *), void *start_arg, const char *name);

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
extern kspinlock_t nk_lock;
/* callouts lock, acquired at IPL=high */
extern kspinlock_t callouts_lock;
/*! the first thread (idle on cpu0) */
extern kthread_t thread0;
/*! the kernel process */
extern kprocess_t proc0;
/*! the bootstrap CPU */
extern kcpu_t cpu0;

#endif /* THREAD_H_ */
