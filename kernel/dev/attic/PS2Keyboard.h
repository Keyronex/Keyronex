#ifndef KRX_DEV_PS2KEYBOARD_H
#define KRX_DEV_PS2KEYBOARD_H

#include "ddk/DKDevice.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "uacpi/namespace.h"

@class PS2Keyboard;

struct ps2_info {
	uacpi_namespace_node *mouse_node;
	uint16_t cmd_port, data_port;
	uint8_t gsi, mouse_gsi;
};

struct ps2_kb_state {
	PS2Keyboard *dev;
	struct ps2_info info;
	kdpc_t dpc;
	uint8_t scancodeBuf[64];
	uint8_t head, tail, count;
	bool isShifted, isCtrled, isExtended, isCapsLocked;
};

@interface PS2Keyboard : DKDevice {
	struct ps2_kb_state m_state;
	struct intr_entry m_intrEntry;
}

+ (BOOL)probeWithProvider:(DKACPIPlatform *)provider
	   acpiNode:(uacpi_namespace_node *)node;

@end

#endif /* KRX_DEV_PS2KEYBOARD_H */
