/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSNumber.h
 * @brief Libkern number class.
 */

#ifndef ECX_LIBKERN_OSNUMBER_H
#define ECX_LIBKERN_OSNUMBER_H

#include <libkern/OSObject.h>
#include <stdint.h>

@interface OSNumber : OSObject {
	uint64_t value;
	size_t size;
}

+ (instancetype)numberWithUInt8:(uint8_t)value;
+ (instancetype)numberWithUInt16:(uint16_t)value;
+ (instancetype)numberWithUInt32:(uint32_t)value;
+ (instancetype)numberWithUInt64:(uint64_t)value;

- (instancetype)initWithUInt64:(uint64_t)value withSize:(uint32_t)bytes;
- (uint64_t)unsignedValue;
- (uint32_t)size;
- (uintptr_t)hash;

@end

#endif /* ECX_LIBKERN_OSNUMBER_H */
