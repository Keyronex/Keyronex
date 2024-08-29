#ifndef KRX_DEV_PS2KEYBOARD_H
#define KRX_DEV_PS2KEYBOARD_H

#include "ddk/DKDevice.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "uacpi/namespace.h"

struct ps2_info {
	uacpi_namespace_node *mouse_node;
	uint16_t cmd_port, data_port;
	uint8_t gsi, mouse_gsi;
};

@interface PS2Keyboard : DKDevice {
	struct ps2_info m_info;

	struct intr_entry m_intrEntry;
	kdpc_t m_dpc;

	uint8_t m_scancodeBuf[64];
	uint8_t m_head, m_tail, m_count;

	bool isShifted, isCtrled, isExtended, isCapsLocked;
}

+ (BOOL)probeWithProvider:(DKACPIPlatform *)provider
	   acpiNode:(uacpi_namespace_node *)node;

@end

#endif /* KRX_DEV_PS2KEYBOARD_H */
