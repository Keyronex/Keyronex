/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSString.h
 * @brief Libkern string class.
 */

#ifndef ECX_LIBKERN_OSSTRING_H
#define ECX_LIBKERN_OSSTRING_H

#include <libkern/OSObject.h>

@interface OSString : OSObject {
	char *string;
	unsigned int length;
}

- (uintptr_t)hash;

@end

@interface OSConstantString : OSString
@end

#endif /* ECX_LIBKERN_OSSTRING_H */
