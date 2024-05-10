#ifndef KRX_KDK_EXECUTIVE_H
#define KRX_KDK_EXECUTIVE_H

#include "kdk/nanokern.h"

/*!
 * Work to be carried out by a worker queue.
 */
typedef struct ex_work {
	TAILQ_ENTRY(ex_work) entry;
	void (*callback)(void *);
	void *parameter;
} ex_work_t;

/*!
 * Executive process
 */
typedef struct eprocess {
	kprocess_t kprocess;
	struct vm_procstate *vm;
} eprocess_t;

int ex_work_enqueue(ex_work_t *work);

int ps_process_create(eprocess_t **out, bool fork);
int ps_thread_create(kthread_t **out, const char *name, void (*fn)(void *),
    void *arg, eprocess_t *process);
int ps_create_kernel_thread(kthread_t **out, const char *name,
    void (*fn)(void *), void *arg);
void ps_exit_this_thread(void);

#define ex_curproc() containerof(curproc(), eprocess_t, kprocess)

extern eprocess_t *kernel_process;

#endif /* KRX_KDK_EXECUTIVE_H */
