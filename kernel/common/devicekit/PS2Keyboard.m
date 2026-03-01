/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Mar 28 2023.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file PS2Keyboard.m
 * @brief PS/2 keyboard driver.
 */

#include <sys/k_log.h>

#include <devicekit/acpi/DKACPINode.h>
#include <devicekit/DKPlatformRoot.h>
#include <devicekit/PS2Keyboard.h>

#if defined (__amd64__)
#include <asm/io.h>
#endif

#include <uacpi/resources.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>

enum {
	kPS2OutputBufferFull = 0x1,
	kPS2InputBufferFull = 0x2,
};

/* kcon.c */
void console_input(const char *buf, int count);

static void dpc_handler(void *arg, void *);
static bool intr_handler(void *arg);

static const char codes[128] = { '\0', '\e', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', '0', '-', '=', '\b', '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u',
	'i', 'o', 'p', '[', ']', '\r', '\0', 'a', 's', 'd', 'f', 'g', 'h', 'j',
	'k', 'l', ';', '\'', '`', '\0', '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm',
	',', '.', '/', '\0', '\0', '\0', ' ', '\0' };

static const char codes_shifted[] = { '\0', '\e', '!', '@', '#', '$', '%', '^',
	'&', '*', '(', ')', '_', '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y',
	'U', 'I', 'O', 'P', '{', '}', '\r', '\0', 'A', 'S', 'D', 'F', 'G', 'H',
	'J', 'K', 'L', ':', '"', '~', '\0', '|', 'Z', 'X', 'C', 'V', 'B', 'N',
	'M', '<', '>', '?', '\0', '\0', '\0', ' ' };

@implementation PS2Keyboard

- (instancetype)initWithACPINode:(DKACPINode *)node
{
	if ((self = [super init])) {
		m_acpiNode = node;
	}
	return self;
}

static uacpi_iteration_decision
resource_cb(void *user, uacpi_resource *resource)
{
	struct ps2_info *info = user;

	if (resource->type == UACPI_RESOURCE_TYPE_IRQ) {
		info->gsi = resource->irq.irqs[0];

	} else if (resource->type == UACPI_RESOURCE_TYPE_FIXED_IO) {
		if (info->cmd_port == (uint16_t)-1)
			info->cmd_port = resource->fixed_io.address;
		else
			info->data_port = resource->fixed_io.address;

	} else if (resource->type == UACPI_RESOURCE_TYPE_IO) {
		if (info->cmd_port == (uint16_t)-1)
			info->cmd_port = resource->io.minimum;
		else
			info->data_port = resource->io.minimum;
	}

	return UACPI_ITERATION_DECISION_CONTINUE;
}

- (void)start
{
	static const uacpi_char *const mouse_ids[] = { "PNP0F03", "PNP0F13",
		"VMW0003", NULL };
	uacpi_resources *resources;
	struct ps2_info ps2_info = { .cmd_port = (uint16_t)-1,
		.data_port = (uint16_t)-1,
		.gsi = -1,
		.mouse_node = NULL,
		.mouse_gsi = -1 };
	int r;

	r = uacpi_get_current_resources([m_acpiNode nsNode], &resources);
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_for_each_resource(resources, resource_cb, &ps2_info);
	kassert(r == UACPI_STATUS_OK);

	uacpi_free_resources(resources);

	if (ps2_info.mouse_node) {
#if 0
		r = uacpi_get_current_resources(info->mouse_node, &resources);
		kassert(r == UACPI_STATUS_OK);

		r = uacpi_for_each_resource(resources, mouse_resource_cb, info);
		kassert(r == UACPI_STATUS_OK);

		uacpi_free_resources(resources);
#endif
	}

	m_state.info = ps2_info;
	m_state.head = m_state.tail = m_state.count = 0;
	m_state.ipl = IPL_HIGH;

	ke_dpc_init(&m_state.dpc, dpc_handler, &m_state, NULL);

	/* fixme: take edge/lopol from acpi */
	kirq_source_t source = {
		.source = ps2_info.gsi,
		.edge = true,
		.low_polarity = false,
	};


	[gPlatformRoot handleSource:&source
			withHandler:intr_handler
			   argument:&m_state
			 atPriority:&m_state.ipl
			  irqObject:&m_state.irqEntry];
}

static void
translateScancode(struct ps2_kb_state *state, uint8_t code)
{
	switch (code) {
	case 0x2a:
	case 0x36:
	case 0xaa:
	case 0xb6:
		state->isShifted = code & 0x80 ? false : true;
		return;

	case 0x1d:
	case 0x9d:
		state->isCtrled = code & 0x80 ? false : true;
		return;

	case 0x4b: /* left */
		return console_input("\e[D", sizeof("\e[D"));

	case 0x4d: /* right */
		return console_input("\e[C", sizeof("\e[C"));

	case 0x48: /* up */
		return console_input("\e[A", sizeof("\e[A"));

	case 0x50: /* down */
		return console_input("\e[B", sizeof("\e[B"));

	default:
		/* epsilon */
		break;
	}

	if (!(code & 0x80)) {
		char ch;

		if (state->isShifted)
			ch = codes_shifted[code];
		else
			ch = codes[code];

		if (state->isCtrled) {
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			ch -= 64;
		}

		console_input(&ch, 1);
	}
}

static void
dpc_handler(void *arg, void *)
{
	struct ps2_kb_state *state = arg;

	while (true) {
		ipl_t ipl = splraise(state->ipl);
		uint8_t val;

		if (state->count == 0) {
			splx(ipl);
			break;
		}

		val = state->scancodeBuf[state->tail++];
		state->tail %= sizeof(state->scancodeBuf);
		state->count--;
		splx(ipl);

		translateScancode(state, val);
	}
}

static bool
intr_handler(void *arg)
{
	struct ps2_kb_state *state = arg;
	uint8_t sc;

#if defined (__amd64__)
	if (!(inb(state->info.data_port) & kPS2OutputBufferFull))
		return false;

	sc = inb(0x60);
#else
	kfatal("PS2Keyboard: implement intr for non-amd64");
#endif

	if (state->count == sizeof(state->scancodeBuf))
		kfatal("out of space in ring buffer");

	state->scancodeBuf[state->head++] = sc;
	state->head %= sizeof(state->scancodeBuf);
	state->count++;

	ke_dpc_schedule(&state->dpc);

	return true;
}

@end
