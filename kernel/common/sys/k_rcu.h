#ifndef ECX_SYS_RCU_H
#define ECX_SYS_RCU_H

#include <sys/k_intr.h>

#include <libkern/queue.h>

/* marker for RCU protected fields */
#define KRX_RCU

typedef void (*krcu_callback_t)(void *);
TAILQ_HEAD(ki_krcu_entry_queue, krcu_entry);

typedef struct krcu_entry {
	TAILQ_ENTRY(krcu_entry) queue_entry;
	krcu_callback_t callback;
	void *arg;
} krcu_entry_t;

struct kep_rcu_per_cpu_data {
	/* members accessed only at IPL >= DPC and only by one core. */
	uintptr_t generation;
	struct ki_krcu_entry_queue past_callbacks, current_callbacks,
	    next_callbacks;
	kdpc_t past_callbacks_dpc;
};

void ke_rcu_call(krcu_entry_t *head, krcu_callback_t callback, void *arg);
void ke_rcu_synchronise(void);
#define ke_rcu_read_lock() spldisp()
#define ke_rcu_read_unlock(IPL_) splx(IPL_)

/*! void ke_rcu_assign_pointer(T KRX_RCU *ptr, T value) */
#define ke_rcu_assign_pointer(PTR, VAL) \
	__atomic_store_n(&(PTR), VAL, __ATOMIC_RELEASE);

/*! T *ke_rcu_dereference(T KRX_RCU *ptr) */
#define ke_rcu_dereference(PTR) __atomic_load_n(&(PTR), __ATOMIC_CONSUME);

/*! T ke_rcu_exchange_pointer(T KRX_RCU *ptr, T value) */
#define ke_rcu_exchange_pointer(PTR, VAL) \
	__atomic_exchange_n(&(PTR), VAL, __ATOMIC_ACQ_REL);

#endif /* ECX_SYS_RCU_H */
