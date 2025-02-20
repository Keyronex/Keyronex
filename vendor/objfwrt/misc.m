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

#include "config.h"

#include <kdk/kern.h>

#include <stdarg.h>

#include "ObjFWRT.h"
#include "private.h"

#ifdef OF_WINDOWS
# include <windows.h>
#endif

#ifdef OF_AMIGAOS
# define Class IntuitionClass
# define USE_INLINE_STDARG
# include <proto/exec.h>
# include <clib/debug_protos.h>
# define __NOLIBBASE__
# include <proto/intuition.h>
# undef __NOLIBBASE__
# undef Class
#endif

static objc_enumeration_mutation_handler enumerationMutationHandler = NULL;

void
objc_enumerationMutation(id object)
{
	if (enumerationMutationHandler != NULL)
		enumerationMutationHandler(object);
	else
		OBJC_ERROR("Object was mutated during enumeration!");
}

void
objc_setEnumerationMutationHandler(objc_enumeration_mutation_handler handler)
{
	enumerationMutationHandler = handler;
}

void
objc_error(const char *title, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	kprintf("[objc] %s: ", title);
	kvpprintf(format, args);
	kfatal("Stopping due to Objective-C error");
	va_end(args);
}

char *
objc_strdup(const char *string)
{
	char *copy;
	size_t length = strlen(string);

	if ((copy = (char *)malloc(length + 1)) == NULL)
		return NULL;

	memcpy(copy, string, length + 1);

	return copy;
}
