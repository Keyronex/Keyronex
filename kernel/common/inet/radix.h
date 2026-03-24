/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Mar 24 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file radix.h
 * @brief Radix tree interface.
 */

#ifndef ECX_INET_RADIX_H
#define ECX_INET_RADIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIX_KEY_MAX	16	/* enough for IPv6 */

enum radix_node_kind {
	RADIX_LINK,
	RADIX_DATA,
};

typedef struct radix_node {
	struct radix_node *child[2];
	struct radix_node *parent;
	enum radix_node_kind kind: 8;
	uint8_t	prefix[RADIX_KEY_MAX];
	uint8_t	prefix_len;	/* in bits */
	void	*data;		/* user-managed data */
} radix_node_t;

typedef struct radix_tree {
	radix_node_t	*root;
	size_t		 key_bytes;	/* key width */
} radix_tree_t;

/* initialise a radix tree, with keys a given width */
void radix_tree_init(radix_tree_t *, size_t key_bytes);

/* destroy all nodes, optionally calling destroy on each data node */
void radix_tree_destroy(radix_tree_t *, void (*destroy)(void *));

/* return data node for this prefix, if it exists, else NULL */
radix_node_t *radix_lookup(const radix_tree_t *, const uint8_t *prefix,
    uint8_t prefix_len);

/* find node matching longest prefix of key, or NULL if no match */
radix_node_t *radix_longest_match(const radix_tree_t *, const uint8_t *key);

/* find or create data node for this prefix */
radix_node_t *radix_insert(radix_tree_t *, const uint8_t *prefix,
    uint8_t prefix_len);

/* remove the data node for the given prefix, returns true if one existed */
bool radix_remove(radix_tree_t *, const uint8_t *prefix,
    uint8_t prefix_len);

/* remove a data node by pointer */
void radix_remove_node(radix_tree_t *, radix_node_t *node);

#endif /* ECX_INET_RADIX_H */
