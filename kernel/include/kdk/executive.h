#ifndef KRX_KDK_EXECUTIVE_H
#define KRX_KDK_EXECUTIVE_H

#include "kdk/nanokern.h"

/*!
 * Boot-time configuration.
 *
 * @var ex_boot_config::root Root FS mount info.
 */
struct ex_boot_config {
	const char *root;
};

/*!
 * Work to be carried out by a worker queue.
 */
typedef struct ex_work {
	TAILQ_ENTRY(ex_work) entry;
	void (*callback)(void *);
	void *parameter;
} ex_work_t;

/*!
 * Executive RWLock
 */
typedef struct ex_rwlock {
	kmutex_t mutex; /* todo! */
} ex_rwlock_t;

/*!
 * Executive process
 */
typedef struct eprocess {
	kprocess_t kprocess;
	struct vm_procstate *vm;
	void *handles[64];
} eprocess_t;

int ex_work_enqueue(ex_work_t *work);

#define ex_rwlock_acquire_read(RWLOCK_, REASON_) \
	ke_wait(&(RWLOCK_)->mutex, REASON_, false, false, -1)

#define ex_rwlock_release_read(RWLOCK_) \
	ke_mutex_release(&(RWLOCK_)->mutex)

#define ex_rwlock_acquire_write(RWLOCK_, REASON_) \
	ex_rwlock_acquire_read(RWLOCK_, REASON_)

#define ex_rwlock_release_write(RWLOCK_) \
	ex_rwlock_release_read(RWLOCK_)

void ps_early_init(kthread_t *thread0);
int ps_process_create(eprocess_t **out, bool fork);
int ps_thread_create(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg, eprocess_t *process);
int ps_create_kernel_thread(kthread_t **out, const char *name,
    void (*fn)(void *), void *arg);
void ps_exit_this_thread(void);

#define ex_proc_from_kproc(KPROC) containerof((KPROC), eprocess_t, kprocess)
#define ex_curproc() containerof(curproc(), eprocess_t, kprocess)

extern struct ex_boot_config boot_config;
extern eprocess_t *kernel_process;

#endif /* KRX_KDK_EXECUTIVE_H */
