/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */

#include "kdk/kernel.h"

#define NANOPRINTF_IMPLEMENTATION
#include <nanoprintf/nanoprintf.h>

kspinlock_t dprintf_lock = KSPINLOCK_INITIALISER;
kcpu_t cpu_bsp;
kcpu_t **all_cpus;
size_t ncpus;

ipl_t
splraise(ipl_t spl)
{
	ipl_t oldspl = splget();
	kassert(oldspl <= spl);
	if (oldspl < spl)
		splx(spl);
	return oldspl;
}
