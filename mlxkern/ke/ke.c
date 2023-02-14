/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */

#include "ke/ke.h"

#define NANOPRINTF_IMPLEMENTATION
#include <nanoprintf/nanoprintf.h>

kspinlock_t dprintf_lock = KSPINLOCK_INITIALISER;
kcpu_t cpu_bsp;

ipl_t
splraise(ipl_t spl)
{
	ipl_t oldspl = splget();
	kassert(oldspl <= spl);
	if (oldspl < spl)
		splx(spl);
	return oldspl;
}
