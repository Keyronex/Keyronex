/*!
 * @file fork.c
 * @brief Implements the "Fork Process Memory" operation.
 */

#include "kdk/executive.h"
#include "vmp.h"

int
do_fork(eprocess_t *parent, eprocess_t *child)
{
	/*
	 * First, acquire exclusively the map locks of parent and child.
	 * This inhibits the changing of their maps and the creation of new
	 * private pages.
	 */
	ex_rwlock_acquire_write(&parent->vm->map_lock, "fork parent");
	ex_rwlock_acquire_write(&child->vm->map_lock, "fork child");
}
