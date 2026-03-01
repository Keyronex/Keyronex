/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Tue Mar 28 2023.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file PS2Keyboard.h
 * @brief PS/2 keyboard.
 */

#ifndef ECX_DEVICEKIT_PS2KEYBOARD_H
#define ECX_DEVICEKIT_PS2KEYBOARD_H

#include <sys/k_intr.h>

#include <devicekit/DKDevice.h>

@class DKACPINode;
@class PS2Keyboard;

struct ps2_info {
	DKACPINode *mouse_node;
	uint16_t cmd_port, data_port;
	uint8_t gsi, mouse_gsi;
};

struct ps2_kb_state {
	struct ps2_info info;
	ipl_t ipl;
	kirq_t irqEntry;
	kdpc_t dpc;
	uint8_t scancodeBuf[64];
	uint8_t head, tail, count;
	bool isShifted, isCtrled, isExtended, isCapsLocked;
};

@interface PS2Keyboard : DKDevice {
	DKACPINode *m_acpiNode;
	struct ps2_kb_state m_state;
}

- (instancetype)initWithACPINode:(DKACPINode *)node;

@end

#endif /* ECX_DEVICEKIT_PS2KEYBOARD_H */
