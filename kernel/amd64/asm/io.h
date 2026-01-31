/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file io.h
 * @brief Port I/O routines.
 */

#ifndef ECX_ASM_IO_H
#define ECX_ASM_IO_H

#include <stdint.h>

static inline void
outb(uint16_t port, uint8_t val)
{
	asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t val;

	asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));

	return val;
}

static inline void
outw(uint16_t port, uint16_t val)
{
	asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t
inw(uint16_t port)
{
	uint16_t val;

	asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));

	return val;
}

static inline void
outl(uint16_t port, uint32_t val)
{
	asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t
inl(uint16_t port)
{
	uint32_t val;

	asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));

	return val;
}

#endif /* ECX_ASM_IO_H */
