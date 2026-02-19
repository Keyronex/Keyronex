/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file devfs.c
 * @brief Device filesystem
 */

#include <sys/k_log.h>
#include <sys/kmem.h>

#include <fs/devfs/devfs.h>

void
devfs_create_node(dev_class_t *class, const char *fmt, ...)
{
	va_list ap;
	dev_node_t *node;

	node = kmem_alloc(sizeof(*node));

	va_start(ap, fmt);
	kvsnprintf(node->name, sizeof(node->name) - 1, fmt, ap);
	va_end(ap);

	kdprintf("devfs_create_node: created %s\n", node->name);
}
