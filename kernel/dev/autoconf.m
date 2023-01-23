/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

//#include <dev/LimineFB.h>
#include <dev/ACPIPC.h>
#include <kern/kmem.h>
#include <amd64/amd64.h>
//#include <dev/fbterm/FBTerminal.h>
//#include <x86_64/boot.h>

//#include "kern/kmem.h"

extern void (*init_array_start)(void);
extern void (*init_array_end)(void);

void
setup_objc(void)
{
	for (void (**func)(void) = &init_array_start; func != &init_array_end;
	     func++)
		(*func)();
}

enum nodeKind { kRoot, kChild, kLastChild };

static void
printTree(DKDevice *dev, char *prefix, enum nodeKind kind)
{
#if 0
	const char *cross = "+";
	const char *rcorner = "\\";
	const char *vline = "| ";
#else
	const char *branch = "\e(0\x78\x71\e(B";  /* ├─ */
	const char *rcorner = "\e(0\x6d\x71\e(B"; /* └─ */
	const char *vline = "\e(0\x78\e(B";	  /* │ */
#endif
	DKDevice *child;
	char     *newPrefix;

	if (kind == kRoot) {
		/* epsilon */
		newPrefix = prefix;
	}
	if (kind == kLastChild) {
		kprintf("%s%s", prefix, rcorner);
		kmem_asprintf(&newPrefix, "%s%s", prefix, "  ");
	} else if (kind == kChild) {
		kprintf("%s%s", prefix, branch);
		kmem_asprintf(&newPrefix, "%s%s", prefix, vline);
	}

	kprintf("%s (class %s)\n", [dev name], [dev classNameCString]);

	TAILQ_FOREACH (child, &dev->m_subDevices, m_subDevices_entry) {
		printTree(child, newPrefix,
		    TAILQ_NEXT(child, m_subDevices_entry) ? kChild :
							    kLastChild);
	}

	if (newPrefix != prefix) {
		// kmem_strfree(prefix);
	}
}

int
autoconf(void)
{
	char indent[255] = { 0 };
	setup_objc();

	kprintf("DeviceKit version 0\n");

	[AcpiPC probeWithRSDP:rsdp_request.response->address];
#if 0
	[LimineFB probeWithProvider:[AcpiPC instance]
		   limineFBResponse:framebuffer_request.response];
	[FBTerminal probeWithFB:sysfb];
#endif

	printTree([AcpiPC instance], indent, kRoot);
	return 0;
}
