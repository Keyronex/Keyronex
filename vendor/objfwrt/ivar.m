/*
 * Copyright (c) 2008-2023 Jonathan Schleifer <js@nil.im>
 *
 * All rights reserved.
 *
 * This file is part of ObjFW. It may be distributed under the terms of the
 * Q Public License 1.0, which can be found in the file LICENSE.QPL included in
 * the packaging of this file.
 *
 * Alternatively, it may be distributed under the terms of the GNU General
 * Public License, either version 2 or 3, which can be found in the file
 * LICENSE.GPLv2 or LICENSE.GPLv3 respectively included in the packaging of this
 * file.
 */

#include "config.h"

#import "ObjFWRT.h"
#import "private.h"

Ivar *
class_copyIvarList(Class class, unsigned int *outCount)
{
	unsigned int count;
	Ivar *ivars;

	if (class == Nil) {
		if (outCount != NULL)
			*outCount = 0;

		return NULL;
	}

	objc_globalMutex_lock();

	count = (class->ivars != NULL ? class->ivars->count : 0);

	if (count == 0) {
		if (outCount != NULL)
			*outCount = 0;

		objc_globalMutex_unlock();
		return NULL;
	}

	if ((ivars = malloc((count + 1) * sizeof(Ivar))) == NULL)
		OBJC_ERROR("Not enough memory to copy ivars!");

	for (unsigned int i = 0; i < count; i++)
		ivars[i] = &class->ivars->ivars[i];
	ivars[count] = NULL;

	if (outCount != NULL)
		*outCount = count;

	objc_globalMutex_unlock();

	return ivars;
}

const char *
ivar_getName(Ivar ivar)
{
	return ivar->name;
}

const char *
ivar_getTypeEncoding(Ivar ivar)
{
	return ivar->typeEncoding;
}

ptrdiff_t
ivar_getOffset(Ivar ivar)
{
	return ivar->offset;
}
