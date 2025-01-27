/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSDictionary.h
 * @brief Maps unique keys to values.
 */

#ifndef ECX_LIBKERN_OSDICTIONARY_H
#define ECX_LIBKERN_OSDICTIONARY_H

#include <libkern/OSObject.h>
#include <libkern/OSMapTable.h>
#include <stddef.h>

struct bucket;

@interface OSDictionary : OSObject {
	OSMapTable *m_table;
}

- (void)setObject:(id)object forKey:(id)key;
- (id)objectForKey:(id)key;

@end

#endif /* ECX_LIBKERN_OSDICTIONARY_H */
