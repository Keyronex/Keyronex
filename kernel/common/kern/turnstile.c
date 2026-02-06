/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file turnstile.c
 * @brief Turnstiles
 */

#include <libkern/queue.h>

typedef struct kturnstile {
	LIST_ENTRY(kturnstile) hash_llink;
	struct kturnstile *free_link;
} kturnstile_t;
