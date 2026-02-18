/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpu.h
 * @brief CPU-local data and related definitions.
 */

#ifndef ECX_KEYRONEX_ATOMIC_H
#define ECX_KEYRONEX_ATOMIC_H

#include <stdint.h>

#if !defined(__OBJC__)

#include <stdatomic.h>

#else

#define _Atomic(...) __VA_ARGS__

typedef bool atomic_bool;
typedef unsigned int atomic_uint;
typedef uintptr_t atomic_uintptr_t;
typedef uint_fast32_t atomic_uint_fast32_t;

#endif

#endif /* ECX_KEYRONEX_ATOMIC_H */
