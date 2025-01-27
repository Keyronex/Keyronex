/*
 * Copyright (c) 2023-25 NetaScale Object Solutions.
 * Created on Wed Sep 20 2023.
 */

#include <kdk/kern.h>

void
ddk_init(void)
{
	extern void (*init_array_start)(void), (*init_array_end)(void);

	kprintf("ddk_init: DeviceKit version 3 for Keyronex-lite\n");

	for (void (**func)(void) = &init_array_start; func != &init_array_end;
	     func++)
		(*func)();
}

void
ddk_early_init(void)
{
	kprintf("ddk_early_init\n");
}

void
ddk_autoconf(void)
{
	kprintf("ddk_autoconf\n");
}
