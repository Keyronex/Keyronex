#include <stddef.h>

#include "ddk/DKDevice.h"
#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "ntcompat/ntcompat.h"

enum nodeKind { kRoot, kChild, kLastChild };

extern void (*init_array_start)(void);
extern void (*init_array_end)(void);
extern void platform_init();

Class platformDeviceClass = NULL;
DKDevice *platformDevice;
static char indent[255] = { 0 };

static void
printTree(DKDevice *dev, char *prefix, enum nodeKind kind)
{
#if 0
	const char *branch = "+-";
	const char *rcorner = "\\-";
	const char *vline = "| ";
#else
	const char *branch = "\e(0\x74\x71\e(B";  /* ├─ */
	const char *rcorner = "\e(0\x6d\x71\e(B"; /* └─ */
	const char *vline = "\e(0\x78\e(B";	  /* │ */
#endif
	DKDevice *child;
	char *newPrefix;

	if (kind == kRoot) {
		/* epsilon */
		newPrefix = prefix;
	}
	if (kind == kLastChild) {
		kprintf("%s%s", prefix, rcorner);
		kmem_asprintf(&newPrefix, "%s%s", prefix, "  ");
	} else if (kind == kChild) {
		kprintf("%s%s", prefix, branch);
		kmem_asprintf(&newPrefix, "%s%s ", prefix, vline);
	}

	kprintf("%s (class %s)\n", [dev devName], [dev className]);

	TAILQ_FOREACH (child, &dev->m_subDevices, m_subDevices_entry) {
		printTree(child, newPrefix,
		    TAILQ_NEXT(child, m_subDevices_entry) ? kChild :
							    kLastChild);
	}

	if (newPrefix != prefix) {
		// kmem_strfree(prefix);
	}
}

void *
malloc(size_t size)
{
	return kmem_malloc(size);
}

void ddk_init(void)
{
	kprintf("ddk_init: DeviceKit version 3 for Keyronex-lite\n");

	for (void (**func)(void) = &init_array_start; func != &init_array_end;
	     func++)
		(*func)();
}

void
ddk_autoconf(void)
{
	kprintf("ddk_init: probing platform device...\n");
	kassert(platformDeviceClass != nil);
	[platformDeviceClass probe];

	kprintf("ddk_init: device tree after autoconf:\n");
	printTree(platformDevice, indent, kRoot);
}
