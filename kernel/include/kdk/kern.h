/*
 * Lock hierarchy - acquire in this order:
 *  - scheduler_lock
 *  - kcpugroup_t::lock
 *  - kcpu_t::dpc_lock
 */

#ifndef KRX_PAC_PAC_H
#define KRX_PAC_PAC_H

#include <stdbool.h>

#include "kdk/nanoprintf.h"
#include "kdk/port.h"
#include "kdk/queue.h"

#define kNThreadWaitBlocks 4

#define KRX_RCU

#define NS_PER_S 1000000000
#define NS_PER_MS 1000000

#define MIN2(a, b) (((a) < (b)) ? (a) : (b))
#define MAX2(a, b) (((a) > (b)) ? (a) : (b))
#define elementsof(x) (sizeof(x) / sizeof((x)[0]))
#define containerof(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);   \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#define ROUNDUP(ADDR, ALIGN) (((ADDR) + ALIGN - 1) & ~(ALIGN - 1))
#define ROUNDDOWN(ADDR, ALIGN) ((((uintptr_t)ADDR)) & ~(ALIGN - 1))
#define PGROUNDUP(ADDR) ROUNDUP(ADDR, PGSIZE)
#define PGROUNDDOWN(ADDR) ROUNDDOWN(ADDR, PGSIZE)

typedef uint64_t nanosecs_t;

typedef struct kspinlock {
	unsigned char flag;
} kspinlock_t;

#define KSPINLOCK_INITIALISER {	\
	0 			\
}

enum kreschedule_reason {
	kRescheduleReasonNone,
	kRescheduleReasonPreempted,
	kRescheduleReasonTimesliceEnd,
};

typedef enum kwaitresult {
	/*! the wait condition was met */
	kKernWaitStatusOK = 0,
	/*! the wait timed out */
	kKernWaitStatusTimedOut = -1,
	/*! invalid argument */
	kKernWaitStatusInvalidArgument = -2,
	/*! thread signalled */
	kKernWaitStatusSignalled = -3,
	/*! internal status - wait is currently underway */
	kKernWaitStatusWaiting = -4,
} kwaitresult_t;

typedef enum kinternalwaitstatus {
	kInternalWaitStatusPreparing,
	kInternalWaitStatusWaiting,
	kInternalWaitStatusSatisfied,
} kinternalwaitstatus_t;

typedef enum kwaitblockstatus {
	kWaitBlockStatusActive,
	kWaitBlockStatusDeactivated,
	kWaitBlockStatusAcquired,
} kwaitblockstatus_t;

typedef TAILQ_HEAD(kwaitblock_queue, kwaitblock) kwaitblock_queue_t;

typedef struct kwaitblock {
	/*! link in kdispatchheader_t::waitblock_queue  */
	TAILQ_ENTRY(kwaitblock) queue_entry;

	/*! status of the wait block */
	kwaitblockstatus_t block_status;
	/*! status of the waiter - atomically accessed */
	kinternalwaitstatus_t *waiter_status;
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
	kDispatchEvent,
	kDispatchPort,
} kdispatchobjecttype_t;

typedef struct kdispatchheader {
	/*! blocks waiting on this thing */
	kwaitblock_queue_t waitblock_queue;

	/*! state spinlock */
	kspinlock_t spinlock;
	/*! dispatch object type */
	kdispatchobjecttype_t type : 3;
	/*! signalled status; >= 1 means currently signaled */
	int signalled;
} kdispatchheader_t;


#define KDISPATCHHEADER_INITIALIZER(HDR_, TYPE_, SIGNALED_) { \
	.spinlock = KSPINLOCK_INITIALISER,	\
	.type = TYPE_,				\
	.signalled = SIGNALED_,			\
	.waitblock_queue = TAILQ_HEAD_INITIALIZER((HDR_).waitblock_queue) \
}

typedef struct kevent {
	kdispatchheader_t hdr;
} kevent_t;

#define KEVENT_INITIALISER(KEVENT_) {		\
	.hdr = KDISPATCHHEADER_INITIALIZER(((KEVENT_).hdr), kDispatchEvent, 0), \
}

typedef struct ksemaphore {
	kdispatchheader_t hdr;
} ksemaphore_t;

typedef struct kmutex {
	kdispatchheader_t hdr;

	/*! thread currently owning the mutex */
	struct kthread *owner;
} kmutex_t;

#define KMUTEX_INITIALIZER(KMUTEX_) {		\
	.hdr = KDISPATCHHEADER_INITIALIZER(((KMUTEX_).hdr), kDispatchMutex, 1), \
	.owner = NULL, \
}

/*!
 * Deferred Procedure Call
 */
typedef struct kdpc {
	/*! Linkage in kcpu_t::dpc_queue */
	TAILQ_ENTRY(kdpc) queue_entry;
	void (*callback)(void *);
	void *arg;
	/*! If non-null, the CPU this DPC is enqueued to be run on. */
	struct kcpu *cpu;
} kdpc_t;

/*!
 * Timer
 */
typedef struct ktimer {
	kdispatchheader_t hdr;
	/*! Current state of timer. Atomically manipulated. */
	enum ktimer_state {
		kTimerDisabled,
		kTimerInQueue,
		kTimerExecuting
	} state;
	/*! Linkage in kcpu_t::timer_queue */
	TAILQ_ENTRY(ktimer) queue_entry;
	/*! Absolute time (in terms of cpu->ticks) of expiry. */
	nanosecs_t deadline;
	/*! If it's pending, on which CPU? */
	struct kcpu *cpu;
	/*! Optional DPC to enqueue on timer expiry */
	kdpc_t *dpc;
} ktimer_t;

/*!
 * Fixed-size message queue.
 */
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

TAILQ_HEAD(kport_msg_queue, kport_msg);

typedef struct kport_msg {
	TAILQ_ENTRY(kport_msg) queue_entry;
} kport_msg_t;

typedef struct kport {
	kdispatchheader_t hdr;
	struct kport_msg_queue queue;
	size_t n_processing;
	size_t max_n_processing;
} kport_t;

TAILQ_HEAD(kthread_queue, kthread);

struct runqueue {
	kspinlock_t lock;
	struct kthread_queue queue;
};

typedef void (*krcu_callback_t)(void *);
TAILQ_HEAD(ki_krcu_entry_queue, krcu_entry);

typedef struct krcu_entry {
	TAILQ_ENTRY(krcu_entry) queue_entry;
	krcu_callback_t callback;
	void *arg;
} krcu_entry_t;

struct ki_rcu_per_cpu_data {
	/*! Both members accessed only at IPL >= DPC and only by one core. */
	uintptr_t generation;
	struct ki_krcu_entry_queue past_callbacks, current_callbacks,
	    next_callbacks;
	kdpc_t past_callbacks_dpc;
};


struct ki_scheduler {
	kspinlock_t lock;
	struct kthread_queue runqueue;
};

/*!
 * Locking:
 * (~) = invariant
 * (d) = IPL high + kcpu_t::dpc_lock
 * (r) = kcpu_t::sched_lock
 * (!) = atomic
 */
typedef struct kcpu {
	md_cpucb_t cpucb;

	/*! (~) */
	int num;

	/*! scheduling lock */
	kspinlock_t sched_lock;
	/*! (r) ready threads queues */
	struct kthread_queue runqueue;
	/*! (r) current thread */
	struct kthread *curthread;
	/*! (r) previous thread - may be stale! */
	struct kthread *old_thread;
	/* (~) this core's idle thread */
	struct kthread *idle_thread;
	/* (!) reason for reschedule if any */
	enum kreschedule_reason reschedule_reason;

	/*! Lock on dpc_queue. */
	kspinlock_t dpc_lock;
	/*! (d) DPCs awaiting execution */
	TAILQ_HEAD(, kdpc) dpc_queue;
	/*! (d) Timers awaiting expiry */
	TAILQ_HEAD(, ktimer) timer_queue;
	/*! Timer expiration processing DPC */
	kdpc_t timer_expiry_dpc;

	/*! (a) Nanoseconds since CPU start */
	nanosecs_t nanos;

	/*! DPC for thread deletion. */
	kdpc_t done_thread_dpc;

	/*! RCU state */
	struct ki_rcu_per_cpu_data rcu_cpustate;
} kcpu_t;

typedef enum kthread_state {
	/*! Thread has been created but not yet put to scheduling. */
	kThreadStateInitial,
	/*! Thread is ready to run. */
	kThreadStateRunnable,
	/*! Thread is currently running on a CPU. */
	kThreadStateRunning,
	/*! Thread is waiting on objects. */
	kThreadStateWaiting,
	/*! Thread has exited and will no longer be scheduled. */
	kThreadStateDone,
} kthread_state_t;

/*!
 * (~) => invariant from creation
 * (!) => atomic
 * (D) => last_cpu->sched_lock
 * (t) => thread lock
 */
typedef struct kthread {
	/*! (~) name */
	const char *name;
	/*! (~) is it a userland thread? */
	bool user;

	kspinlock_t lock;

	/*! (~) process to which it belongs */
	struct kprocess *process;
	/*! entry in run/waitqueue */
	TAILQ_ENTRY(kthread) queue_link;
	/*! entry in kprocess::thread_list */
	LIST_ENTRY(kthread) list_link;
	/*! Machine-specific context. */
	md_pcb_t pcb;
	/*! Kernel stack base. */
	void *kstack_base;
	/*! (t?) CPU this thread last ran on */
	struct kcpu *last_cpu;
	/*! (t, sometimes?) current or soon-to-happen thread state */
	kthread_state_t state;
	/*! (!) remaining timeslice */
	int8_t timeslice;

	/*! built-in waitblocks */
	kwaitblock_t integral_waitblocks[kNThreadWaitBlocks];
	/*! waitblocks list being currently waited on */
	kwaitblock_t *waitblocks;
	/*! kind of wait */
	bool iswaitall;
	/* number of objects being waited on */
	size_t nwaits;
	/*! wait timeout timer */
	ktimer_t wait_timer;
	/*! wait reason */
	const char *wait_reason;
	/*! internal wait status information */
	kinternalwaitstatus_t wait_status;

	/*! port this thread is currently processing a message on */
	kport_t *port;
	/*! message received on port */
	kport_msg_t *port_msg;

	/* TCB */
	uintptr_t tcb;
	/* ID - only needed for userland threads. to move into ethread. */
	uintptr_t tid;
} kthread_t;

typedef enum kprocess_state {
	/*! Process is live and contains (or will contain) threads. */
	kProcessStateLive,
	/*! Process has no remaining threads and has ended. */
	kProcessStateTerminated,
} kprocess_state_t;

/*!
 * (~) = invariant from creation
 * (l) = lock
 */
typedef struct kprocess {
	/*! process lock */
	kspinlock_t lock;
	/*! (l) thread count; when it *drops* to 0, process terminates */
	uint32_t thread_count;
	/*! (l) process state */
	kprocess_state_t state;
	/*! (l) threads of process */
	LIST_HEAD(, kthread) thread_list;
} kprocess_t;

/*! @brief Get pointer to this core. */
#define curcpu() ({		      \
	curthread()->last_cpu;        \
})
/*! @brief Get pointer to current kprocess. */
#define curproc() curthread()->process

/*!
 * Machine-dependent routines.
 */

/*!
 * Platform debug console putchar.
 */
void pac_putc(int ch, void *unused);

/*!
 * Switch this core's current thread.
 *
 * \pre curcpu()->curthread set to new thread
 * \pre IPL=high.
 *
 * \post IPL=high
 */
void md_switch(kthread_t *old_thread);

/*!
 * @brief Raise interrupt priority level and return the previous.
 */
ipl_t splraise(ipl_t ipl);

/*!
 * @brief Lower the interrupt priority level.
 */
void splx(ipl_t ipl);

/*!
 * @brief Get the current interrupt priority level.
 */
ipl_t splget(void);

/*!
 * Ends machine-dependent routines.
 */

#define spldpc() splraise(kIPLDPC)

extern kspinlock_t idle_lock;

#if 1 // SMP == 1
/*! @brief Determine if a spinlock is held. */
static inline bool
ke_spinlock_held(kspinlock_t *lock)
{
	return __atomic_load_n(&lock->flag, __ATOMIC_ACQUIRE) == 1;
}

/*! @brief Acquire a spinlock without SPL */
static inline void
ke_spinlock_acquire_nospl(kspinlock_t *lock)
{
	unsigned char zero = 0;
	unsigned char one = 1;
	while (!__atomic_compare_exchange(&lock->flag, &zero, &one, 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		zero = 0;
	}
}

/*! @brief Release a spinlock without SPL */
static inline void
ke_spinlock_release_nospl(kspinlock_t *lock)
{
	unsigned char zero = 0;
	__atomic_store(&lock->flag, &zero, __ATOMIC_RELEASE);
}
#else
/*! @brief Determine if a spinlock is held - noop on UP */
static inline bool
ke_spinlock_held(kspinlock_t *lock)
{
	return true;
}

/*! @brief Acquire a spinlock without SPL - noop on UP */
static inline void
ke_spinlock_acquire_nospl(kspinlock_t *lock)
{
}

/*! @brief Release a spinlock without SPL */
static inline void
ke_spinlock_release_nospl(kspinlock_t *lock)
{
}
#endif

/*! @brief Initialise a spinlock */
static inline void
ke_spinlock_init(kspinlock_t *lock)
{
	ke_spinlock_release_nospl(lock);
}

/*! @brief Acquire a spinlock at DPC IPL */
static inline ipl_t
ke_spinlock_acquire(kspinlock_t *lock)
{
	ipl_t ipl = spldpc();
	ke_spinlock_acquire_nospl(lock);
	return ipl;
}

/*! @brief Acquire a spinlock at custom IPL */
static inline ipl_t
ke_spinlock_acquire_at(kspinlock_t *lock, ipl_t ipl)
{
	ipl_t oldipl = splraise(ipl);
	ke_spinlock_acquire_nospl(lock);
	return oldipl;
}

/*! @brief Release a spinlock and restore saved IPL */
static inline void
ke_spinlock_release(kspinlock_t *lock, ipl_t oldipl)
{
	ke_spinlock_release_nospl(lock);
	splx(oldipl);
}

void ke_dpc_enqueue(kdpc_t *dpc);

/*!
 * @brief Initialise a kernel event, either signalled or nonsignalled.
 */
void ke_event_init(kevent_t *ev, bool signalled);

/*!
 * @brief Signal a kernel event.
 */
void ke_event_signal(kevent_t *ev);

/*!
 * @brief Test if an event is signalled.
 */
bool ke_event_is_signalled(kevent_t *ev);

/*!
 * @brief Clear a kernel event, resetting it to nonsignalled state.
 * @returns the previous signal state of the event.
 */
bool ke_event_clear(kevent_t *ev);

/*!
 * @brief Initialise a kernel messagequeue with the given capacity.
 */
void ke_msgqueue_init(kmsgqueue_t *msgq, unsigned count);

/*!
 * @brief Post a message to a kernel messagequeue. Waits until it can be done.
 */
void ke_msgq_post(kmsgqueue_t *queue, void *msg);

/*!
 * @brief Read a message from a kernel messagequeue.
 * @retval 0 a message was retrieved
 * @retval 1 no messages were pending
 */
int ke_msgq_read(kmsgqueue_t *queue, void **msg);

/*!
 * @brief Initialise a kernel mutex.
 */
void ke_mutex_init(kmutex_t *mutex);

/*!
 * @brief Release a kernel mutex.
 */
void ke_mutex_release(kmutex_t *mutex);

/*!
 * @brief Assert the mutex is held by the current thread.
 */
#define ke_mutex_assert_held(MUTEX) (kassert((MUTEX)->owner == curthread()))

/*!
 * @brief Initialise a port.
 */
void ke_port_init(kport_t *port);

void ke_process_init(kprocess_t *proc);

/*!
 * @brief Initialise a kernel semaphore.
 * \p count Initial count.
 */
void ke_semaphore_init(ksemaphore_t *sem, unsigned count);

/*!
 * @brief Reset a semaphore to a given count.
 * \p count Count to which to reset the semaphore.
 */
void ke_semaphore_reset(ksemaphore_t *sem, unsigned count);

/*!
 * Release a kernel semaphore.
 *
 * \p semaphore Pointer to a kernel semaphore.
 * \p adjustment Value to add to the semaphore count.
 */
void ke_semaphore_release(ksemaphore_t *sem, unsigned adjustment);

/*!
 * Release a kernel semaphore, adding 1 to the count only if count was 0.
 *
 * \p semaphore Pointer to a kernel semaphore.
 * \p adjustment Value to add to the semaphore count.
 */
void ke_semaphore_release_maxone(ksemaphore_t *sem);

/*!
 * @brief Initialise a kernel timer.
 */
void ke_timer_init(ktimer_t *timer);

/*!
 * @brief Set a kernel timer for a given time.
 */
void ke_timer_set(ktimer_t *timer, uint64_t nanosecs);

/*!
 * @brief Cancel a pending timer or wait until it has completed.
 */
void ke_timer_cancel(ktimer_t *timer);

void ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg);

void ke_thread_resume(kthread_t *thread);

void ke_thread_deinit(kthread_t *thread);

kwaitresult_t ke_wait(void *object, const char *reason, bool isuserwait,
    bool alertable, nanosecs_t timeout);
kwaitresult_t ke_wait_multi(size_t nobjects, void *objects[],
    const char *reason, bool isWaitall, bool isUserwait, bool isAlertable,
    nanosecs_t timeout, kwaitblock_t *waitblocks);

#define STRINGIFY(x) #x
#define KE_WAIT(OBJ, USER, ALERTABLE, TIMEOUT)                               \
	ke_wait(OBJ, #OBJ "(at " __FILE__ ":" STRINGIFY(__LINE__) ")", USER, \
	    ALERTABLE, TIMEOUT)

void ke_rcu_call(krcu_entry_t *head, krcu_callback_t callback, void *arg);
void ke_rcu_synchronise(void);
#define ke_rcu_read_lock() spldpc()
#define ke_rcu_read_unlock(IPL_) splx(IPL_)

/*! void ke_rcu_assign_pointer(T KRX_RCU *ptr, T value) */
#define ke_rcu_assign_pointer(PTR, VAL) \
	__atomic_store_n(&(PTR), VAL, __ATOMIC_RELEASE);

/*! T *ke_rcu_dereference(T KRX_RCU *ptr) */
#define ke_rcu_dereference(PTR) __atomic_load_n(&(PTR), __ATOMIC_CONSUME);

/*! T ke_rcu_exchange_pointer(T KRX_RCU *ptr, T value) */
#define ke_rcu_exchange_pointer(PTR, VAL) \
	__atomic_exchange_n(&(PTR), VAL, __ATOMIC_ACQ_REL);

/* Kernel putc. */
void kputc(int ch, void *unused);

/* Platform debug console printf */
#define printf_wrapper(PUTC, ...) ({					 \
	ipl_t _ipl = ke_spinlock_acquire_at(&pac_console_lock, kIPLHigh);\
	int _r = npf_pprintf(PUTC, NULL, __VA_ARGS__);		 	 \
	ke_spinlock_release(&pac_console_lock, _ipl);			 \
	_r;								 \
})

#define vpprintf_wrapper(PUTC, ...) ({                         \
	ipl_t _ipl = ke_spinlock_acquire_at(&pac_console_lock, \
	    kIPLHigh);                                         \
	int _r = npf_vpprintf(PUTC, NULL, __VA_ARGS__);        \
	ke_spinlock_release(&pac_console_lock, _ipl);          \
	_r;                                                    \
})

#define pac_printf(...) printf_wrapper(pac_putc, __VA_ARGS__)
#define pac_vpprintf(...) vpprintf_wrapper(pac_putc, __VA_ARGS__)

#define kprintf(...) printf_wrapper(kputc, __VA_ARGS__)
#define kvpprintf(...) vpprintf_wrapper(kputc, __VA_ARGS__)

/*! @brief Kernel fatal - prints out on PAC and main console and halts. */
#define kfatal(...) {				\
	kprintf("at %s:%d (%s):\n", __FILE__,\
	    __LINE__, __FUNCTION__); 		\
	kprintf(__VA_ARGS__); 		\
	hcf();					\
}

/*! @brief Kernel assertion - prints out on PAC console. */
#define kassert(...) {						\
	if (!(__VA_ARGS__)) {					\
		kfatal("kernel assertion failed:\n\t%s\n",	\
		    #__VA_ARGS__);				\
	}							\
}

#define kassert_dbg(...) kassert(__VA_ARGS__)

void ke_format_time(nanosecs_t nanosecs, char *out, size_t size);

void ke_set_tcb(uintptr_t tcb);

#if SMP == 1
extern kcpu_t **cpus;
#else
extern kcpu_t *cpus[1];
#endif
/*! NUmber of CPUs in system. */
extern size_t ncpus;

extern kspinlock_t pac_console_lock;

#endif /* KRX_PAC_PAC_H */
