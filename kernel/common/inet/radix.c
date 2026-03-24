/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Mar 24 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file radix.c
 * @brief Radix tree implementation
 */

#include <sys/kmem.h>
#include <sys/libkern.h>

#include <inet/radix.h>
#include "sys/k_log.h"

static bool
bit_test(const uint8_t *key, size_t n)
{
	return (key[n / 8] & (uint8_t)(0x80U >> (n % 8))) != 0;
}

static uint8_t
common_prefix_len(const uint8_t *a, uint8_t a_len,
    const uint8_t *b, uint8_t b_len)
{
	uint8_t limit = a_len < b_len ? a_len : b_len;

	for (uint8_t i = 0; i < limit; i++) {
		if (bit_test(a, i) != bit_test(b, i))
			return i;
	}
	return limit;
}

static void
mask_prefix(uint8_t *dst, const uint8_t *src, size_t nbytes,
    uint8_t prefix_len)
{
	size_t full;

	memcpy(dst, src, nbytes);
	full = prefix_len / 8;
	if (full < nbytes && prefix_len % 8 != 0) {
		dst[full] &= (uint8_t)(0xFFU << (8 - prefix_len % 8));
		full++;
	}
	if (full < nbytes)
		memset(dst + full, 0, nbytes - full);
}

/* does node's prefix match first node->prefix_len bits of key? */
static bool
node_matches(const radix_node_t *node, const uint8_t *key,
    size_t key_bits)
{
	return common_prefix_len(node->prefix, node->prefix_len,
	    key, key_bits) >= node->prefix_len;
}

/*
 * node allocation
 */

static radix_node_t *
node_alloc(const uint8_t *prefix, uint8_t prefix_len, size_t key_bytes,
    enum radix_node_kind kind)
{
	radix_node_t *n;

	n = kmem_zalloc(sizeof(*n));
	mask_prefix(n->prefix, prefix, key_bytes, prefix_len);
	n->prefix_len = prefix_len;
	n->kind = kind;
	return n;
}

static void
node_free(radix_node_t *n)
{
	kmem_free(n, sizeof(*n));
}

static void
set_child(radix_node_t *parent, int side, radix_node_t *child)
{
	if (parent != NULL)
		parent->child[side] = child;
	if (child != NULL)
		child->parent = parent;
}

/*
 * tree lifetime
 */

void
radix_tree_init(radix_tree_t *tree, size_t key_bytes)
{
	kassert(key_bytes > 0 && key_bytes <= RADIX_KEY_MAX);

	tree->root = NULL;
	tree->key_bytes = key_bytes;
}

void
radix_tree_destroy(radix_tree_t *tree, void (*destroy)(void *))
{
	radix_node_t *cur= tree->root;
	radix_node_t *parent;

	while (cur != NULL) {
		if (cur->child[0] != NULL) {
			cur = cur->child[0];
			continue;
		}
		if (cur->child[1] != NULL) {
			cur = cur->child[1];
			continue;
		}

		/* now a leaf */
		parent = cur->parent;

		if (cur->kind == RADIX_DATA && destroy != NULL &&
		    cur->data != NULL)
			destroy(cur->data);

		if (parent != NULL) {
			if (parent->child[0] == cur)
				parent->child[0] = NULL;
			else
				parent->child[1] = NULL;
		}

		node_free(cur);
		cur = parent;
	}

	tree->root = NULL;
}

/*
 * api
 */

radix_node_t *
radix_lookup(const radix_tree_t *tree, const uint8_t *prefix,
    uint8_t prefix_len)
{
	radix_node_t *node;
	size_t maxbits;

	kassert(tree != NULL && prefix != NULL);
	kassert(prefix_len <= tree->key_bytes * 8);

	maxbits = tree->key_bytes * 8;
	node = tree->root;

	while (node != NULL) {
		if (!node_matches(node, prefix, maxbits))
			return NULL;

		if (node->prefix_len == prefix_len) {
			/* verify full prefix equality */
			uint8_t masked[RADIX_KEY_MAX];
			mask_prefix(masked, prefix, tree->key_bytes,
			    prefix_len);
			if (memcmp(node->prefix, masked,
			    tree->key_bytes) == 0 && node->kind == RADIX_DATA)
				return node;
			return NULL;
		}

		if (prefix_len <= node->prefix_len)
			return NULL;

		node = bit_test(prefix, node->prefix_len) ?
		    node->child[1] : node->child[0];
	}
	return NULL;
}

radix_node_t *
radix_longest_match(const radix_tree_t *tree, const uint8_t *key)
{
	radix_node_t *node;
	radix_node_t *best = NULL;
	size_t maxbits;

	kassert(tree != NULL && key != NULL);

	maxbits = tree->key_bytes * 8;
	node = tree->root;

	while (node != NULL) {
		if (!node_matches(node, key, maxbits))
			break;
		if (node->kind == RADIX_DATA)
			best = node;
		if (node->prefix_len >= maxbits)
			break;
		node = bit_test(key, node->prefix_len) ?
		    node->child[1] : node->child[0];
	}
	return best;
}

radix_node_t *
radix_insert(radix_tree_t *tree, const uint8_t *prefix,
    uint8_t prefix_len)
{
	radix_node_t **link;
	radix_node_t *node;
	radix_node_t *parent;
	radix_node_t *newnode;
	radix_node_t *branch;
	uint8_t common;
	uint8_t masked[RADIX_KEY_MAX];

	kassert(tree != NULL && prefix != NULL);
	kassert(prefix_len <= tree->key_bytes * 8);

	mask_prefix(masked, prefix, tree->key_bytes, prefix_len);

	link = &tree->root;
	parent = NULL;
	node = tree->root;

	while (node != NULL) {
		common = common_prefix_len(node->prefix, node->prefix_len,
		    masked, prefix_len);

		if (common == node->prefix_len &&
		    common == prefix_len) {
			/* exact match, promote from link to data if needed */
			node->kind = RADIX_DATA;
			return node;
		}

		if (common == node->prefix_len &&
		    common < prefix_len) {
			/* node is ancestor, descend */
			parent = node;
			if (bit_test(masked, node->prefix_len)) {
				link = &node->child[1];
				node = node->child[1];
			} else {
				link = &node->child[0];
				node = node->child[0];
			}
			continue;
		}

		if (common == prefix_len &&
		    common < node->prefix_len) {
			/* new prefix is ancestor of node, insert above */
			newnode = node_alloc(masked, prefix_len,
			    tree->key_bytes, true);
			newnode->parent = parent;
			if (bit_test(node->prefix, prefix_len))
				set_child(newnode, 1, node);
			else
				set_child(newnode, 0, node);
			*link = newnode;
			return newnode;
		}

		/* neither is ancestor, new parent needed at common prefix */
		branch = node_alloc(masked, common, tree->key_bytes, false);
		branch->parent = parent;

		newnode = node_alloc(masked, prefix_len,
		    tree->key_bytes, true);

		if (bit_test(masked, common)) {
			set_child(branch, 0, node);
			set_child(branch, 1, newnode);
		} else {
			set_child(branch, 0, newnode);
			set_child(branch, 1, node);
		}
		*link = branch;
		return newnode;
	}

	/* empty, create a new leaf */
	newnode = node_alloc(masked, prefix_len, tree->key_bytes, true);
	newnode->parent = parent;
	*link = newnode;
	return newnode;
}

/* collapse needless internal nodes upwards */
static void
collapse(radix_tree_t *tree, radix_node_t *start)
{
	radix_node_t *node;
	radix_node_t *parent;
	radix_node_t *child;

	node = start;
	while (node != NULL && node->kind == RADIX_LINK &&
	    (node->child[0] == NULL || node->child[1] == NULL)) {
		parent = node->parent;
		child = node->child[0] != NULL ?
		    node->child[0] : node->child[1];

		if (parent == NULL) {
			tree->root = child;
			if (child != NULL)
				child->parent = NULL;
		} else if (parent->child[0] == node) {
			set_child(parent, 0, child);
		} else {
			set_child(parent, 1, child);
		}

		node->child[0] = NULL;
		node->child[1] = NULL;
		node_free(node);
		node = parent;
	}
}

void
radix_remove_node(radix_tree_t *tree, radix_node_t *node)
{
	radix_node_t *parent;
	radix_node_t *child;

	kassert(tree != NULL && node != NULL);
	kassert(node->kind == RADIX_DATA);

	node->kind = RADIX_LINK;
	node->data = NULL;

	if (node->child[0] != NULL && node->child[1] != NULL) {
		/* node still needed as a link */
		return;
	}

	/* splice out the node and collapse upwards */
	parent = node->parent;
	child = node->child[0] != NULL ?
	    node->child[0] : node->child[1];

	if (parent == NULL) {
		tree->root = child;
		if (child != NULL)
			child->parent = NULL;
	} else if (parent->child[0] == node) {
		set_child(parent, 0, child);
	} else {
		set_child(parent, 1, child);
	}

	node->child[0] = NULL;
	node->child[1] = NULL;
	node_free(node);

	collapse(tree, parent);
}

bool
radix_remove(radix_tree_t *tree, const uint8_t *prefix,
    uint8_t prefix_len)
{
	radix_node_t *node;

	node = radix_lookup(tree, prefix, prefix_len);
	if (node == NULL)
		return false;

	radix_remove_node(tree, node);
	return true;
}
