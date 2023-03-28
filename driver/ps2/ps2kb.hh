/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Mar 28 2023.
 */

#ifndef KRX_PS2_PS2KB_H
#define KRX_PS2_PS2KB_H

#include "kdk/amd64/mdamd64.h"

#include "../acpipc/acpipc.hh"

class PS2Keyboard : public Device {
	bool isShifted;
	bool isCtrled;
	bool isExtended;
	bool isCapsLocked;

	intr_entry intrEntry;

	uint16_t dataIo, cmdIo;

	/*! Ring buffer for deferred processing. */
	uint8_t buf[128];
	uint8_t head;
	uint8_t tail;
	uint8_t count;

	/*! Deferred processing */
	kdpc_t dpc;

	static bool intrHandler(hl_intr_frame_t *frame, void *arg);
	static void dpcHandler(void *arg);

	void translateScancode(uint8_t val);

    public:
	PS2Keyboard(AcpiPC *provider, lai_nsnode_t *node);
};

#endif /* KRX_PS2_PS2KB_H */
