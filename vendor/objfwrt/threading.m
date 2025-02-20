/*
 * Copyright (c) 2008-2024 Jonathan Schleifer <js@nil.im>
 *
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3.0 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * version 3.0 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * version 3.0 along with this program. If not, see
 * <https://www.gnu.org/licenses/>.
 */
#if 0
#include "config.h"

#import "ObjFWRT.h"
#import "private.h"

#import "OFOnce.h"
#import "OFPlainMutex.h"

static OFPlainRecursiveMutex globalMutex;
#endif

static void
init(void)
{
#if 0
	if (OFPlainRecursiveMutexNew(&globalMutex) != 0)
		OBJC_ERROR("Failed to create global mutex!");
#endif
}

void
objc_globalMutex_lock(void)
{
#if 0
	static OFOnceControl onceControl = OFOnceControlInitValue;
	OFOnce(&onceControl, init);

	if (OFPlainRecursiveMutexLock(&globalMutex) != 0)
		OBJC_ERROR("Failed to lock global mutex!");
#endif
}

void
objc_globalMutex_unlock(void)
{
#if 0
	if (OFPlainRecursiveMutexUnlock(&globalMutex) != 0)
		OBJC_ERROR("Failed to unlock global mutex!");
#endif
}
