/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Mar 28 2023.
 */

#include "kdk/amd64/mdamd64.h"
#include "kdk/amd64/portio.h"
#include "kdk/kmem.h"
#include "lai/helpers/resource.h"

#include "../acpipc/ioapic.hh"
#include "ps2kb.hh"

enum {
	kPS2OutputBufferFull = 0x1,
	kPS2InputBufferFull = 0x2,
};

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

int
laiex_view_resource(lai_nsnode_t *node, lai_variable_t *crs,
    struct lai_resource_view *view, lai_state_t *state)
{
	lai_nsnode_t *hcrs;

	hcrs = lai_resolve_path(node, "_CRS");

	if (hcrs == NULL) {
		lai_warn("missing _CRS\n");
		return -1;
	}

	if (lai_eval(crs, hcrs, state)) {
		lai_warn("failed to eval _CRS");
		return -1;
	}

	*view = (struct lai_resource_view)LAI_RESOURCE_VIEW_INITIALIZER(crs);

	return 0;
}

PS2Keyboard::PS2Keyboard(AcpiPC *provider, lai_nsnode_t *node)
{
	LAI_CLEANUP_VAR lai_variable_t crs = LAI_VAR_INITIALIZER;
	struct lai_resource_view res;
	LAI_CLEANUP_STATE lai_state_t state;
	lai_api_error_t err;
	int ioa = -1, iob = -1, gsi = -1;
	int r;

	lai_init_state(&state);
	if (laiex_view_resource(node, &crs, &res, &state) < 0)
		return;

	while ((err = lai_resource_iterate(&res)) == 0) {
		enum lai_resource_type type = lai_resource_get_type(&res);
		if (type == LAI_RESOURCE_IO) {
			if (ioa == -1)
				ioa = res.base;
			else
				iob = res.base;
		} else if (type == LAI_RESOURCE_IRQ) {
			while (lai_resource_next_irq(&res) !=
			    LAI_ERROR_END_REACHED) {
				if (gsi == -1)
					gsi = res.gsi;
				else {
					DKLog("PS2Keyboard",
					    "strange number of IRQs, gsi is %lu\n",
					    res.entry_idx);
					break;
				}
			}
		}
	}

	if (ioa == -1 || iob == -1 || gsi == -1) {
		DKLog("PS2Keyboard",
		    "failed to identify resources from ACPI\n");
		return;
	}

	dpc.arg = this;
	dpc.callback = dpcHandler;
	dpc.state = kdpc::kDPCUnbound;

	dataIo = ioa;
	cmdIo = iob;

	head = 0;
	tail = 0;
	count = 0;

	r = IOApic::handleGSI(gsi, intrHandler, this, false, false, kIPLDevice,
	    &intrEntry);
	if (r != 0) {
		DKLog("PS2Keyboard", "Failed to set up interrupt: %d\n", r);
		return;
	}

	kmem_asprintf(&objhdr.name, "pckbd0");
	attach(provider);

	DKDevLog(this, "I/O 0x%x,0x%x IRQ %d\n", ioa, iob, gsi);
}

bool
PS2Keyboard::intrHandler(hl_intr_frame_t *frame, void *arg)
{
	PS2Keyboard *self = (PS2Keyboard *)arg;
	uint8_t in;

	if (!(inb(self->cmdIo) & kPS2OutputBufferFull))
		return false;

	in = inb(0x60);
	if (self->count == sizeof(self->buf))
		kfatal("out of space in ring buffer");
	self->buf[self->head++] = in;
	self->head %= sizeof(self->buf);
	self->count++;

	ke_dpc_enqueue(&self->dpc);

	return true;
}

void
PS2Keyboard::dpcHandler(void *arg)
{
	PS2Keyboard *self = (PS2Keyboard *)arg;

	while (true) {
		ipl_t ipl = splraise(kIPLDevice);
		uint8_t val;

		if (self->count == 0) {
			splx(ipl);
			break;
		}

		val = self->buf[self->tail++];
		self->tail %= sizeof(self->buf);
		self->count--;
		splx(ipl);

		self->translateScancode(val);
	}
}

void
PS2Keyboard::translateScancode(uint8_t code)
{
	switch (code) {
	case 0x2a:
	case 0x36:
	case 0xaa:
	case 0xb6:
		isShifted = code & 0x80 ? false : true;
		return;

	case 0x1d:
	case 0x9d:
		isCtrled = code & 0x80 ? false : true;
		return;

	case 0x4b: /* left */
		return syscon_instr("\e[D");

	case 0x4d: /* right */
		return syscon_instr("\e[C");

	case 0x48: /* up */
		return syscon_instr("\e[A");

	case 0x50: /* down */
		return syscon_instr("\e[B");

	default:
	    /* epsilon */
	    ;
	}

	if (!(code & 0x80)) {
		char ch;

		if (isShifted)
			ch = codes_shifted[code];
		else
			ch = codes[code];

		if (isCtrled) {
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			ch -= 64;
		}

		syscon_inchar(ch);
	}
}