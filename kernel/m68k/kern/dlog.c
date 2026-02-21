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

static bool gftty_handler(void *)
{
	uint32_t bytes_ready;

	while ((bytes_ready = gftty_read(GFTTY_BYTES_READY)) > 0) {
		bytes_ready = MIN2(bytes_ready, 256);
		gftty_write(GFTTY_DATA_PTR, (uintptr_t)v2p(buf));
		gftty_write(GFTTY_DATA_LEN, bytes_ready);
		gftty_write(GFTTY_CMD, GFTTY_CMD_READ_BUFFER);
		kdputn(buf, bytes_ready);
	}
	return true;
}

void
gftty_enable_irq(void)
{
	buf = kmem_alloc(256);
	gfpic_handle_irq(31, gftty_handler, NULL);
	gfpic_unmask_irq(31);
	gftty_write(GFTTY_CMD, GFTTY_CMD_INT_ENABLE);
}
