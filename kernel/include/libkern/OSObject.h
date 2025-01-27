/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSObject.h
 * @brief Base class for libkern objects.
 */

#ifndef ECX_LIBKERN_OSOBJECT_H
#define ECX_LIBKERN_OSOBJECT_H

#include <objfwrt/ObjFWRT.h>
#include <stdint.h>

__attribute__((__objc_root_class__))
@interface OSObject {
	Class _isa;
}

+ (void)load;
+ (void)unload;
+ (void)initialize;

+ (instancetype)alloc;
+ (instancetype)new;

+ (Class)class;
+ (const char *)className;

- (void)dealloc;
- (instancetype)init;
- (instancetype)retain;
- (void)release;

- (const char *)className;

- (bool)isEqual:(id)object;
- (bool)isKindOfClass:(Class)class;
- (uintptr_t)hash;

@end

#endif /* ECX_LIBKERN_OSOBJECT_H */
