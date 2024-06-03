#ifndef KRX_DEV_PS2KEYBOARD_H
#define KRX_DEV_PS2KEYBOARD_H

#include "ddk/DKDevice.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "uacpi/namespace.h"

@interface PS2Keyboard : DKDevice {
}

+ (BOOL)probeWithProvider:(DKACPIPlatform *)provider
	   acpiNode:(uacpi_namespace_node *)node;

@end

#endif /* KRX_DEV_PS2KEYBOARD_H */
