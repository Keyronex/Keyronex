/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Wed Jul 03 2024.
 */
/*!
 * @file port_obj.c
 * @brief Implements the "Port" object type, a wrapper for `kport_t`s.
 */

#include <abi-bits/errno.h>

#include "kdk/executive.h"
#include "object.h"

obj_class_t port_class;

void
exp_port_obj_init(void)
{
	port_class = obj_new_type("Port", NULL, true);
}

ex_desc_ret_t
ex_service_port_new(eprocess_t *proc)
{
	int r;
	descnum_t descnum;
	kport_t *port;

	descnum = ex_object_space_reserve(proc->objspace, false);
	if (descnum == DESCNUM_NULL)
		return -EMFILE;

	r = obj_new(&port, port_class, sizeof(kport_t), "a port");
	if (r != 0) {
		ex_object_space_free_index(proc->objspace, descnum);
		return -ENOMEM;
	}

	ke_port_init(port);
	ex_object_space_reserved_insert(proc->objspace, descnum, port);

	return descnum;
}
