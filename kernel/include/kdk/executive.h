#ifndef KRX_KDK_EXECUTIVE_H
#define KRX_KDK_EXECUTIVE_H

#include "kdk/kern.h"

/*!
 * @brief Descriptor number or negative errno value.
 *
 * This is the return type of most executive services that return a descriptor
 * number. Successful results are indicated by a zero or positive value,
 * unsuccessful results by a negative errno.
 */
typedef int32_t ex_desc_ret_t;

/*!
 * @brief Size or negative errno value.
 *
 * This is the return type of most executive services that return a size.
 * Successful results are indicated by a zero or positive value, unsuccessful
 * results by a negative errno.
 */
typedef intptr_t ex_size_ret_t;

/*!
 * @brief Offset or negative errno return value.
 *
 * This is the return type of most executive services that return an offset.
 * Successful results are indicated by a zero or positive value, unsuccessful
 * results by a negative errno.
 */
typedef intptr_t ex_off_ret_t;

/*! @brief Descriptor number indexing into an object table. */
typedef int32_t descnum_t;

/*! @brief NULL-equivalent object descriptor number. */
#define DESCNUM_NULL (descnum_t)-1

/*! @brief Generic object pointer. */
typedef void obj_t;

/*! @brief An object space. */
typedef struct ex_object_space ex_object_space_t;

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
	ex_object_space_t *objspace;
} eprocess_t;

/*! @brief Create a new handle table. */
ex_object_space_t *ex_object_space_create(void);

/*! @brief Resolve an object descriptor number within a space. */
obj_t *ex_object_space_lookup(ex_object_space_t *table, descnum_t descnum);

/*! @brief Reserve an entry within an object space. */
descnum_t ex_object_space_reserve(ex_object_space_t *table, bool cloexec);

/*! @brief Insert an entry into a already reserved slot in an object space. */
void ex_object_space_reserved_insert(ex_object_space_t *table, descnum_t descnum, obj_t *obj);

/*! @brief Free an index in an object space. Returns prior value (if any). */
obj_t *ex_object_space_free_index(ex_object_space_t *table, descnum_t descnum);

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

size_t user_strlen(const char *user_str);
int copyout_str(const char *ustr, char **out);

#define ex_proc_from_kproc(KPROC) containerof((KPROC), eprocess_t, kprocess)
#define ex_curproc() containerof(curproc(), eprocess_t, kprocess)

extern struct ex_boot_config boot_config;
extern eprocess_t *kernel_process;

#endif /* KRX_KDK_EXECUTIVE_H */
