/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sat Feb 11 2023.
 */

#ifndef MLX_KE_KE_H
#define MLX_KE_KE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <bsdqueue/queue.h>
#include <nanoprintf/nanoprintf.h>
#include <stdbool.h>

#include "kdk/machdep.h"
#include "kdk/objhdr.h"

#define kNThreadWaitBlocks 4

typedef struct kspinlock {
	unsigned char flag;
} kspinlock_t;

#define KSPINLOCK_INITIALISER \
	{                     \
		0             \
	}

typedef struct kdpc {
	/*! Linkage in kcpu_t::dpc_queue */
	TAILQ_ENTRY(kdpc) queue_entry;
	void (*callback)(void *);
	void *arg;
} kdpc_t;

typedef enum kwaitstatus {
	/*! the wait condition was met */
	kKernWaitStatusOK,
	/*! the wait timed out */
	kKernWaitStatusTimedOut,
	/*! invalid argument */
	kKernWaitStatusInvalidArgument,
	/*! internal status - wait is currently underway */
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
	kDispatchEvent,
} kdispatchobjecttype_t;

typedef struct kdispatchheader {
	/*! blocks waiting on this thing */
	TAILQ_HEAD(, kwaitblock) waitblock_queue;

	/*! dispatch object type */
	kdispatchobjecttype_t type : 3;
	/*! signalled status */
	int signalled;
} kdispatchheader_t;

typedef struct kevent {
	kdispatchheader_t hdr;
} kevent_t;

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

	/*! Linkage in kcpu_t::timer_queue */
	TAILQ_ENTRY(ktimer) queue_entry;
	/*! DPC to be enqueued on its elapsing */
	kdpc_t dpc;
	/*! Relative time till expiry. */
	nanosecs_t nanosecs;
	/*! Absolute time (in terms of cpu->ticks) of expiry. */
	nanosecs_t deadline;
	/*! State of the timer. */
	enum {
		/*! Timer is not currently enqueued. */
		kTimerDisabled,
		/*! Timer is pending. */
		kTimerPending,
		/*! Timer has elapsed and its DPC has been enqueued. */
		kTimerElapsed,
	} state;
	/*! If kCalloutPending/kCalloutElapsed, on which CPU? */
	struct kcpu *cpu;
} ktimer_t;

TAILQ_HEAD(ktimer_queue, ktimer);

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

struct kthread_stats {
	/*! When did it last start running or waiting? */
	uint64_t last_start_time;
	/*! How long has it been running for in total? */
	uint64_t total_run_time;
	/*! How long has it spent waiting on events? */
	uint64_t total_wait_time;
};

typedef struct kthread {
	/*!
	 * Object manager header. Belongs in ethread_t but is here to preserve
	 * the object header as the first element of the structure.
	 */
	object_header_t objhdr;

	/*! Process thread list linkage. */
	SLIST_ENTRY(kthread) kproc_threads_link;
	/*! Process to which it belongs. */
	struct kprocess *process;

	/*! Thread state. */
	kthread_state_t state;
	/*! Thread's current CPU binding. */
	struct kcpu *cpu;
	/*! Runqueue linkage */
	TAILQ_ENTRY(kthread) runqueue_link;
	/*! Scheduling statistics. */
	struct kthread_stats stats;
	/*! Remaining timeslice. */
	int64_t timeslice;

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
	/*! wait timeout timer */
	ktimer_t wait_timer;
	/*! name */
	const char *name;
	/*! wait reason */
	const char *wait_reason;

	/*! Kernel stack base. */
	vaddr_t kstack;

	/*! IPL before switch. */
	ipl_t saved_ipl;
	/*! Saved state at switch. */
	hl_intr_frame_t frame;

	/*! Platform-specific state. */
	struct hl_thread hl;
} kthread_t;

/*!
 * l => #lock
 */
typedef struct kprocess {
	/*! Object manager header. */
	object_header_t objhdr;

	kspinlock_t lock;
	/*! (l) threads of the process; linked by kthread::kproc_threads_link */
	SLIST_HEAD(, kthread) threads;
} kprocess_t;

enum kreschedule_reason {
	kRescheduleReasonNone,
	kRescheduleReasonPreempted,
};

/*!
 * - ~ => stable from creation onwards
 * - a => atomic
 * - d => dispatcher_lock
 * - D => dpc_lock
 */
typedef struct kcpu {
	/*! (~) Unique identifier. */
	uint32_t num;

	/*! (d) DPC queue */
	TAILQ_HEAD(, kdpc) dpc_queue;
	/*! (d) Scheduler entry reason */
	enum kreschedule_reason reschedule_reason;
	/*! (D) Pending timers queue */
	struct ktimer_queue timer_queue;

	/*! (d) Thread run queue */
	TAILQ_HEAD(, kthread) runqueue;
	/*! (d) Currently running thread */
	kthread_t *current_thread;
	/*! (~) CPU idle thread */
	kthread_t *idle_thread;

	/*! (a) Number of ticks occurred. */
	nanosecs_t ticks;

	unsigned int
	    /*! DPC-level software-interrupt pending */
	    dpc_int : 1;

	/*! Platform-specific data */
	struct hl_cpu hl;
} kcpu_t;

#define ke_curthread() hl_curcpu()->current_thread

/*! @brief Get tick count on a given CPU. */
int64_t ke_get_ticks(kcpu_t *cpu);

/*! @brief Debug printf. */
#define kdprintf(...)                                                        \
	{                                                                    \
		/* kdprintf is usable everywhere, so need SPL high */        \
		ipl_t ipl = ke_spinlock_acquire_at(&dprintf_lock, kIPLHigh); \
		npf_pprintf(hl_dputc, NULL, __VA_ARGS__);                    \
		ke_spinlock_release(&dprintf_lock, ipl);                     \
	}

/*! @brief Kernel fatal condition. */
#define kfatal(...)                                              \
	{                                                        \
		kdprintf("at %s:%d (%s):\n", __FILE__, __LINE__, \
		    __FUNCTION__);                               \
		kdprintf(__VA_ARGS__);                           \
		while (1) {                                      \
			asm("hlt");                              \
		}                                                \
	}

/*! @brief Kernel assertion */
#define kassert(...)                                                           \
	{                                                                      \
		if (!(__VA_ARGS__)) {                                          \
			kfatal("kernel assertion failed: %s\n", #__VA_ARGS__); \
		}                                                              \
	}

/*! @brief Determine if a spinlock is held. */
static inline bool
ke_spinlock_held(kspinlock_t *lock)
{
	return __atomic_load_n(&lock, __ATOMIC_RELAXED);
}

/*! @brief Acquire a spinlock without SPL */
static inline void
ke_spinlock_acquire_nospl(kspinlock_t *lock)
{
	unsigned char zero = 0;
	unsigned char one = 1;
	while (!__atomic_compare_exchange(&lock->flag, &zero, &one, 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		asm("pause");
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

/*! @brief Acquire the dispatcher database lock. */
#define ke_acquire_dispatcher_lock() ke_spinlock_acquire(&dispatcher_lock)

/*! @brief Release the dispatcher database lock. */
#define ke_release_dispatcher_lock(IPL) \
	ke_spinlock_release(&dispatcher_lock, IPL)

/*! @brief Acquire the DPC queues lock. */
#define ke_acquire_dpc_lock() ke_spinlock_acquire_at(&dpc_queues_lock, kIPLHigh)

/*! @brief Release the DPC queues lock. */
#define ke_release_dpc_lock(IPL) ke_spinlock_release(&dpc_queues_lock, IPL)

/*!
 * @brief Periodic timer interrupt handler.
 */
bool ki_cpu_hardclock(hl_intr_frame_t *frame, void *arg);

/*!
 * @brief Reschedule request IPI handler.
 */
bool ki_reschedule_ipi(hl_intr_frame_t *frame, void *arg);

/*!
 * @brief Reschedule the CPU.
 * @pre dispatcher lock held
 * @post dispatcher lock released, IPL remains unchanged
 */
void ki_reschedule(void);

/*! @brief Raise a DPC-level software interrupt. */
void ki_raise_dpc_interrupt(void);

/*! @brief Start a newly-created thread. */
void ki_thread_start(kthread_t *thread);

/*! @brief Enqueue a DPC (will be run immediately if IPL < kIPLDPC) */
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
 * @brief Initialise a kernel semaphore.
 * \p count Initial count.
 */
void ke_semaphore_init(ksemaphore_t *sem, unsigned count);

/*!
 * Release a kernel semaphore.
 *
 * \p semaphore Pointer to a kernel semaphore.
 * \p adjustment Value to add to the semaphore count.
 */
void ke_semaphore_release(ksemaphore_t *sem, unsigned adjustment);

/*!
 * @brief Initialise a kernel timer.
 */
void ke_timer_init(ktimer_t *timer);

/*!
 * @brief Set a kernel timer for a given time.
 */
void ke_timer_set(ktimer_t *timer, uint64_t nanosecs);

kwaitstatus_t ke_wait(void *object, const char *reason, bool isuserwait,
    bool alertable, nanosecs_t timeout);

kwaitstatus_t ke_wait_multi(size_t nobjects, void *objects[],
    const char *reason, bool isWaitall, bool isUserwait, bool isAlertable,
    nanosecs_t timeout, mlx_nullable mlx_out kwaitblock_t *waitblocks);

/*! Platform-specific debug putc(). */
void hl_dputc(int ch, void *ctx);

/*! Dispatcher database lock. */
extern kspinlock_t dispatcher_lock;
/*! DPC queues lock. */
extern kspinlock_t dpc_queues_lock;
/*! Debug print lock. */
extern kspinlock_t dprintf_lock;
/*! Bootstrap CPU. */
extern kcpu_t cpu_bsp;
/*! All CPUs by CPU number. */
extern kcpu_t **all_cpus;
/*! NUmber of CPUs in system. */
extern size_t ncpus;

#ifdef __cplusplus
}
#endif

#endif /* MLX_KE_KE_H */
