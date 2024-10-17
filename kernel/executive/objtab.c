/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sat Jun 29 2024.
 */
/*!
 * @file objtab.c
 * @brief Object descriptor table management.
 */

#include <sys/errno.h>

#include "exp.h"
#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/misc.h"
#include "kdk/object.h"
#include "object.h"

#define KRX_RCU

struct objtab_entries {
	krcu_entry_t rcu_entry;
	uint32_t capacity;	  /*!< capacity of rights array */
	uint32_t n_open;	  /*!< number open or reserved */
	obj_t KRX_RCU **objptrs; /*!< array of object pointers */
	bitmap_32_t *open;	  /*!< bitmap of open or reserved objects */
	bitmap_32_t *cloexec;	  /*!< bitmap of close-on-exec objects */
};

/*!
 * The object space structure.
 *
 * update_lock is used to serialize updates to objptrs corresponding to
 * allocated desciptor numbers against resizes.
 *
 * write_lock is used to serialize allocations/frees of descriptor numbers and
 * all changes to open/cloexec.
 */
struct ex_object_space {
	kmutex_t write_lock;
	kspinlock_t update_lock;
	kevent_t resize_done_event;

	struct objtab_entries KRX_RCU *entries;
};

ex_object_space_t *
ex_object_space_create(ex_object_space_t *fork)
{
	ex_object_space_t *table;
	struct objtab_entries *entries;
	size_t capacity;

	table = kmem_alloc(sizeof(*table));
	if (table == NULL)
		return NULL;

	ke_mutex_init(&table->write_lock);
	ke_spinlock_init(&table->update_lock);
	ke_event_init(&table->resize_done_event, true);

	entries = kmem_alloc(sizeof(*entries));
	if (entries == NULL)
		goto fail_entries;

	if (fork != NULL)
		capacity = fork->entries->capacity;
	else
		capacity = 64;

	entries->capacity = capacity;
	entries->n_open = 0;
	entries->objptrs = kmem_alloc(capacity * sizeof(obj_t *));
	if (entries->objptrs == NULL)
		goto fail;

	entries->open = kmem_alloc(bit32_size(capacity));
	if (entries->open == NULL)
		goto fail;

	entries->cloexec = kmem_alloc(bit32_size(capacity));
	if (entries->cloexec == NULL)
		goto fail;

	if (fork != NULL) {
		for (size_t i = 0; i < capacity; i++) {
			if (bit32_test(fork->entries->open, i)) {
				obj_t *obj = fork->entries->objptrs[i];
				kassert(obj != NULL);
				obj = obj_tryretain_rcu(obj);
				kassert(obj != NULL);
				ke_rcu_assign_pointer(entries->objptrs[i], obj);
			}
		}
		memcpy(entries->open, fork->entries->open,
		    bit32_size(capacity));
		memcpy(entries->cloexec, fork->entries->cloexec,
		    bit32_size(capacity));
	} else {
		memset(entries->objptrs, 0, capacity * sizeof(obj_t *));
		memset(entries->open, 0, bit32_size(capacity));
		memset(entries->cloexec, 0, bit32_size(capacity));
	}

	ke_rcu_assign_pointer(table->entries, entries);

	return table;

fail:
	kmem_free(entries->objptrs, capacity * sizeof(obj_t *));
	kmem_free(entries->open, bit32_size(capacity));
	kmem_free(entries->cloexec, bit32_size(capacity));
	kmem_free(entries, sizeof(*entries));
fail_entries:
	kmem_free(table, sizeof(*table));

	return NULL;
}

obj_t *
ex_object_space_lookup(ex_object_space_t *table, descnum_t descnum)
{
	ipl_t ipl;
	obj_t *right = NULL;
	struct objtab_entries *entries;

	if (descnum < 0)
		return NULL;

	ipl = ke_rcu_read_lock();

	while (true) {
		entries = ke_rcu_dereference(table->entries);

		if (descnum >= entries->capacity ||
		    !bit32_test(entries->open, descnum))
			break;

		right = ke_rcu_dereference(entries->objptrs[descnum]);
		if (right == NULL)
			break; /* raced to close or not yet open */

		right = obj_tryretain_rcu(right);
		if (right != NULL)
			break; /* successfully retained the right */

		/* raced to close or exchange; retry */
	}

	ke_rcu_read_unlock(ipl);

	return right;
}

static struct objtab_entries *
resize_entries(ex_object_space_t *table, struct objtab_entries *entries,
    size_t new_capacity)
{
	struct objtab_entries *new_entries;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&table->update_lock);
	ke_event_clear(&table->resize_done_event);
	ke_spinlock_release(&table->update_lock, ipl);

	new_entries = kmem_alloc(sizeof(*new_entries));
	if (new_entries == NULL)
		goto fail_entries;

	new_entries->capacity = new_capacity;
	new_entries->n_open = entries->n_open;
	new_entries->objptrs = NULL;
	new_entries->open = NULL;
	new_entries->cloexec = NULL;

	new_entries->objptrs = kmem_alloc(new_capacity * sizeof(obj_t *));
	if (new_entries->objptrs == NULL)
		goto fail;

	new_entries->open = kmem_alloc(bit32_size(new_capacity));
	if (new_entries->open == NULL)
		goto fail;

	new_entries->cloexec = kmem_alloc(bit32_size(new_capacity));
	if (new_entries->cloexec == NULL)
		goto fail;

	memcpy(new_entries->objptrs, entries->objptrs,
	    entries->capacity * sizeof(void *));
	memset(new_entries->objptrs + entries->capacity, 0,
	    (new_capacity - entries->capacity) * sizeof(void *));

	memcpy(new_entries->open, entries->open, bit32_size(entries->capacity));
	memset(new_entries->open + bit32_nelem(entries->capacity), 0,
	    bit32_size(new_capacity) - bit32_size(entries->capacity));

	memcpy(new_entries->cloexec, entries->cloexec,
	    bit32_size(entries->capacity));
	memset(new_entries->cloexec + bit32_nelem(entries->capacity), 0,
	    bit32_size(new_capacity) - bit32_size(entries->capacity));

	ipl = ke_spinlock_acquire(&table->update_lock);
	ke_event_signal(&table->resize_done_event);
	ke_spinlock_release(&table->update_lock, ipl);

fail:
	kmem_free(new_entries->objptrs, new_capacity * sizeof(obj_t *));
	kmem_free(new_entries->open, bit32_size(new_capacity));
	kmem_free(new_entries->cloexec, bit32_size(new_capacity));
	kmem_free(new_entries, sizeof(*new_entries));

fail_entries:
	ipl = ke_spinlock_acquire(&table->update_lock);
	ke_event_signal(&table->resize_done_event);
	ke_spinlock_release(&table->update_lock, ipl);

	return NULL;
}

static descnum_t
descnum_alloc_locked(ex_object_space_t *table, bool cloexec)
{
	struct objtab_entries *entries = table->entries;
	descnum_t descnum = -1;

	if (entries->n_open < entries->capacity) {
		for (uint32_t i = 0; i < entries->capacity; i++) {
			if (!bit32_test(entries->open, i)) {
				descnum = i;
				break;
			}
		}
	}

	if (descnum == -1) {
		uint32_t old_capacity, new_capacity;
		struct objtab_entries *new_entries;

		old_capacity = entries->capacity;
		new_capacity = old_capacity * 2;

		new_entries = resize_entries(table, entries, new_capacity);
		if (new_entries == NULL)
			return -1;

		ke_rcu_assign_pointer(table->entries, new_entries);
		entries = new_entries;

		descnum = entries->capacity / 2;
	}

	bit32_set(entries->open, descnum);
	if (cloexec)
		bit32_set(entries->cloexec, descnum);

	entries->n_open++;

	return descnum;
}

descnum_t
ex_object_space_reserve(ex_object_space_t *table, bool cloexec)
{
	descnum_t descnum;

	ke_wait(&table->write_lock, "ex_object_space_reserve", false, false,
	    -1);
	descnum = descnum_alloc_locked(table, cloexec);
	ke_mutex_release(&table->write_lock);

	return descnum;
}

void
ex_object_space_reserved_insert(ex_object_space_t *table, descnum_t descnum,
    obj_t *obj)
{
	ipl_t ipl;
	struct objtab_entries *entries;

	while (true) {
		ipl = ke_spinlock_acquire(&table->update_lock);

		/* this is fine - the update lock serialises things. */
		if (!table->resize_done_event.hdr.signalled) {
			ke_spinlock_release(&table->update_lock, ipl);
			ke_wait(&table->resize_done_event,
			    "ex_object_space_reserved_insert", false, false,
			    -1);
			continue;
		} else {
			break;
		}
	}

	entries = ke_rcu_dereference(table->entries);

	kassert_dbg(descnum < entries->capacity);
	kassert_dbg(bit32_test(entries->open, descnum));

	ke_rcu_assign_pointer(entries->objptrs[descnum], obj);

	ke_spinlock_release(&table->update_lock, ipl);
}

int
ex_object_space_free_index(ex_object_space_t *table, descnum_t descnum,
    obj_t **out)
{
	obj_t *right;
	struct objtab_entries *entries;

	ke_wait(&table->write_lock, "ex_object_space_free_index", false, false,
	    -1);

	entries = ke_rcu_dereference(table->entries);

	if (descnum >= entries->capacity || !bit32_test(entries->open, descnum)) {
		kfatal("%d is not open!\n", descnum);
		return -EBADF;
	}

	entries->n_open--;
	bit32_clear(entries->open, descnum);
	bit32_clear(entries->cloexec, descnum);
	right = ke_rcu_exchange_pointer(entries->objptrs[descnum], NULL);

	ke_mutex_release(&table->write_lock);

	*out = right;

	return 0;
}
