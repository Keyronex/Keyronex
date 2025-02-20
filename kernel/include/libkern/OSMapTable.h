/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */

#ifndef LIBKERN_OSMAPTABLE_H_
#define LIBKERN_OSMAPTABLE_H_

#include <sys/types.h>

#include <stdbool.h>

typedef struct OSMapTable OSMapTable;

struct OSMapKeyCallbacks {
	unsigned long (*hash)(const void *key);
	bool (*isEqual)(const void *key1, const void *key2);
	void *(*retain)(const void *key);
	void (*release)(void *key);
};

struct OSMapValueCallbacks {
	void *(*retain)(const void *value);
	void (*release)(void *value);
};

/*
 * Creates a new map table with the specified callbacks for key and value
 * management. The hash and isEqual callbacks are required; retain and release
 * are optional. Returns NULL if required callbacks are missing or if memory
 * allocation fails.
 */
OSMapTable *OSMapTableCreate(const struct OSMapKeyCallbacks *key_callbacks,
    const struct OSMapValueCallbacks *value_callbacks);

/*
 * Associates the specified value with the specified key in the map table.
 * If the key already exists, the old value is released and replaced.
 * Both key and value are retained before storing.
 */
bool OSMapTableSetValue(OSMapTable *table, const void *key, const void *value);

/*
 * Returns the value associated with the specified key, or NULL if not found.
 * The returned value is not retained; the caller must retain it if needed.
 */
void *OSMapTableGetValue(OSMapTable *table, const void *key);

/*
 * Releases all keys and values, frees all buckets and the table itself.
 */
void OSMapTableDestroy(OSMapTable *table);

/*
 * Returns the number of entries in the map table.
 */
size_t OSMapTableGetCount(const OSMapTable *table);

#endif /* _LIBKERN_OSMAPTABLE_H_ */
