/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log for m68k. Should move to some kind of virt platform
 *   thing instead.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>

#include <stdint.h>

#include "goldfish.h"

void console_input(const char *buf, int count);

enum gftty_reg {
	GFTTY_PUT_CHAR = 0x00,
	GFTTY_BYTES_READY = 0x04,
	GFTTY_CMD = 0x08,
	GFTTY_DATA_PTR = 0x10,
	GFTTY_DATA_LEN = 0x14,
};

enum gftty_cmd {
	GFTTY_CMD_INT_DISABLE = 0x00,
	GFTTY_CMD_INT_ENABLE = 0x01,
	GFTTY_CMD_WRITE_BUFFER = 0x02,
	GFTTY_CMD_READ_BUFFER = 0x03,
};

volatile char *gftty_regs = (void *)0x8000000;
static char *buf;
static kdpc_t gftty_dpc;

/* todo- factor out for other drivers? spsc */
#define GFTTY_RB_SIZE 4096u
#define GFTTY_RB_MASK (GFTTY_RB_SIZE - 1)

static uint8_t gftty_rb_data[GFTTY_RB_SIZE];
static atomic_uint_fast32_t gftty_rb_head;
static atomic_uint_fast32_t gftty_rb_tail;

static inline uint32_t
gftty_rb_free(uint32_t head, uint32_t tail)
{
	return (GFTTY_RB_SIZE - 1u) - ((head - tail) & GFTTY_RB_MASK);
}

static inline uint32_t
gftty_rb_avail(uint32_t head, uint32_t tail)
{
	return (head - tail) & GFTTY_RB_MASK;
}

static uint32_t
gftty_rb_write_irq(const uint8_t *src, uint32_t n)
{
	uint32_t head = atomic_load_explicit(&gftty_rb_head,
	    memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&gftty_rb_tail,
	    memory_order_acquire);
	uint32_t free = gftty_rb_free(head, tail);
	uint32_t to_write = (n < free) ? n : free;
	uint32_t idx = head & GFTTY_RB_MASK;
	uint32_t first = GFTTY_RB_SIZE - idx;

	if (to_write == 0)
		return 0;

	if (first > to_write)
		first = to_write;

	for (uint32_t i = 0; i < first; i++)
		gftty_rb_data[idx + i] = src[i];
	for (uint32_t i = 0; i < (to_write - first); i++)
		gftty_rb_data[i] = src[first + i];

	atomic_store_explicit(&gftty_rb_head, head + to_write,
	    memory_order_release);

	return to_write;
}

static uint32_t
gftty_rb_read_dpc(char *dst, uint32_t n)
{
	uint32_t tail = atomic_load_explicit(&gftty_rb_tail,
	    memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&gftty_rb_head,
	    memory_order_acquire);
	uint32_t avail = gftty_rb_avail(head, tail);
	uint32_t to_read = (n < avail) ? n : avail;
	uint32_t idx = tail & GFTTY_RB_MASK;
	uint32_t first = GFTTY_RB_SIZE - idx;

	if (to_read == 0)
		return 0;

	if (first > to_read)
		first = to_read;

	for (uint32_t i = 0; i < first; i++)
		dst[i] = gftty_rb_data[idx + i];
	for (uint32_t i = 0; i < (to_read - first); i++)
		dst[first + i] = gftty_rb_data[i];

	atomic_store_explicit(&gftty_rb_tail, tail + to_read,
	    memory_order_release);

	return to_read;
}

static void
gftty_write(enum gftty_reg reg, uint32_t val)
{
	*((uint32_t *)&gftty_regs[reg]) = val;
}

static uint32_t
gftty_read(enum gftty_reg reg)
{
	return *((uint32_t *)&gftty_regs[reg]);
}

void
gftty_init(void)
{
	gftty_write(GFTTY_CMD, GFTTY_CMD_INT_DISABLE);
}

void
ke_md_early_putc(int c, void *)
{
	if (c == '\n')
		gftty_write(GFTTY_PUT_CHAR, '\r');
	gftty_write(GFTTY_PUT_CHAR, c);
}

static bool
gftty_handler(void *)
{
	uint32_t bytes_ready;
	bool wrote = false;

	while ((bytes_ready = gftty_read(GFTTY_BYTES_READY)) > 0) {
		bytes_ready = MIN2(bytes_ready, 256);

		gftty_write(GFTTY_DATA_PTR, (uintptr_t)v2p(buf));
		gftty_write(GFTTY_DATA_LEN, bytes_ready);
		gftty_write(GFTTY_CMD, GFTTY_CMD_READ_BUFFER);

		uint32_t written = gftty_rb_write_irq((const uint8_t *)buf, bytes_ready);

		/* optional: count drops if written < bytes_ready */
		if (written > 0)
			wrote = true;
	}

	if (wrote)
		ke_dpc_schedule(&gftty_dpc); /* your scheduler hook */

	return true;
}

static void
gftty_dpchandler(void *, void *)
{
	char buf[256] = {0};

	for (;;) {
		uint32_t n = gftty_rb_read_dpc(buf, sizeof(buf));
		if (n == 0)
			break;

		console_input(buf, n);
	}
}

void
gftty_enable_irq(void)
{
	buf = kmem_alloc(256);

	atomic_init(&gftty_rb_head, 0);
	atomic_init(&gftty_rb_tail, 0);
	ke_dpc_init(&gftty_dpc, gftty_dpchandler, NULL, NULL);

	gfpic_handle_irq(31, gftty_handler, NULL);
	gfpic_unmask_irq(31);
	gftty_write(GFTTY_CMD, GFTTY_CMD_INT_ENABLE);
}
