/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Feb 15 2025.
 */
/*!
 * @file timecounter.c
 * @brief Timecounter implementation (currently minimalistic.)
 */

#include <kdk/kern.h>

#include "kern/ki.h"

static nanosecs_t tc_ticks_get_nanos(void);

static nanosecs_t (*get_nanos)(void) = tc_ticks_get_nanos;

nanosecs_t
ke_get_nanos()
{
	return get_nanos();
}

void
ke_tc_set_get_nanos(nanosecs_t (*func)(void))
{
	get_nanos = func;
}

static nanosecs_t
tc_ticks_get_nanos(void)
{
	return curcpu()->nanos;
}
