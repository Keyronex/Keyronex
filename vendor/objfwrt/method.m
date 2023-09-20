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

Method *
class_copyMethodList(Class class, unsigned int *outCount)
{
	unsigned int i, count;
	struct objc_method_list *iter;
	Method *methods;

	if (class == Nil) {
		if (outCount != NULL)
			*outCount = 0;

		return NULL;
	}

	objc_globalMutex_lock();

	count = 0;
	for (iter = class->methodList; iter != NULL; iter = iter->next)
		count += iter->count;

	if (count == 0) {
		if (outCount != NULL)
			*outCount = 0;

		objc_globalMutex_unlock();
		return NULL;
	}

	if ((methods = malloc((count + 1) * sizeof(Method))) == NULL)
		OBJC_ERROR("Not enough memory to copy methods");

	i = 0;
	for (iter = class->methodList; iter != NULL; iter = iter->next)
		for (unsigned int j = 0; j < iter->count; j++)
			methods[i++] = &iter->methods[j];

	if (i != count)
		OBJC_ERROR("Fatal internal inconsistency!");

	methods[count] = NULL;

	if (outCount != NULL)
		*outCount = count;

	objc_globalMutex_unlock();

	return methods;
}

SEL
method_getName(Method method)
{
	return (SEL)&method->selector;
}

const char *
method_getTypeEncoding(Method method)
{
	return method->selector.typeEncoding;
}
