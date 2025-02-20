/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */

#include <kdk/kmem.h>
#include <libkern/OSMapTable.h>

struct bucket {
	void *key;
	void *value;
	struct bucket *next;
};

struct OSMapTable {
	struct bucket *buckets;
	size_t buckets_n;
	size_t entries_n;
	struct OSMapKeyCallbacks key_callbacks;
	struct OSMapValueCallbacks value_callbacks;
};

OSMapTable *
OSMapTableCreate(const struct OSMapKeyCallbacks *key_callbacks,
    const struct OSMapValueCallbacks *value_callbacks)
{
	OSMapTable *table;

	if (key_callbacks == NULL || value_callbacks == NULL)
		return NULL;

	if (key_callbacks->hash == NULL || key_callbacks->isEqual == NULL)
		return NULL;

	table = kmem_alloc(sizeof(struct OSMapTable));
	if (table == NULL)
		return NULL;

	table->buckets_n = 16;
	table->entries_n = 0;

	table->buckets = kmem_zalloc(sizeof(struct bucket) * table->buckets_n);
	if (table->buckets == NULL) {
		kmem_free(table, sizeof(struct OSMapTable));
		return NULL;
	}

	table->key_callbacks = *key_callbacks;
	table->value_callbacks = *value_callbacks;

	return table;
}

static void
OSMapTableResize(OSMapTable *table)
{
	size_t old_size = table->buckets_n;
	struct bucket *old_buckets = table->buckets;

	table->buckets_n *= 2;

	table->buckets = kmem_zalloc(sizeof(struct bucket) * table->buckets_n);
	if (table->buckets == NULL) {
		kfatal("OSMapTableResize: kmem_zalloc failed");
		table->buckets_n = old_size;
		table->buckets = old_buckets;
		return;
	}

	for (size_t i = 0; i < old_size; i++) {
		struct bucket *bucket = &old_buckets[i];

		if (bucket->key != NULL) {
			unsigned long hash = table->key_callbacks.hash(
			    bucket->key);
			size_t new_index = hash % table->buckets_n;

			struct bucket *new_bucket = &table->buckets[new_index];

			if (new_bucket->key == NULL) {
				new_bucket->key = bucket->key;
				new_bucket->value = bucket->value;
			} else {
				struct bucket *chain_bucket = kmem_alloc(
				    sizeof(struct bucket));
				if (chain_bucket == NULL) {
					kfatal("OSMapTableResize: "
					       "kmem_alloc failed");
				}
				chain_bucket->key = bucket->key;
				chain_bucket->value = bucket->value;
				chain_bucket->next = new_bucket->next;
				new_bucket->next = chain_bucket;
			}
		}

		for (struct bucket *cur = bucket->next, *next; cur != NULL;
		     cur = next) {
			unsigned long hash = table->key_callbacks.hash(cur->key);
			size_t new_index = hash % table->buckets_n;
			struct bucket *new_bucket = &table->buckets[new_index];

			next = cur->next;

			if (new_bucket->key == NULL) {
				new_bucket->key = cur->key;
				new_bucket->value = cur->value;
				new_bucket->next = NULL;
			} else {
				struct bucket *chain_bucket = kmem_alloc(
				    sizeof(struct bucket));
				if (chain_bucket == NULL) {
					kfatal("OSMapTableResize: "
					       "kmem_alloc failed");
					continue;
				}
				chain_bucket->key = cur->key;
				chain_bucket->value = cur->value;
				chain_bucket->next = new_bucket->next;
				new_bucket->next = chain_bucket;
			}

			kmem_free(cur, sizeof(struct bucket));
		}
	}

	kmem_free(old_buckets, sizeof(struct bucket) * old_size);
}

void *
OSMapTableGetValue(OSMapTable *table, const void *key)
{
	unsigned long hash;
	size_t index;

	if (table == NULL || key == NULL)
		return NULL;

	hash = table->key_callbacks.hash(key);
	index = hash % table->buckets_n;

	for (struct bucket *bucket = &table->buckets[index];
	     bucket && bucket->key; bucket = bucket->next) {
		if (table->key_callbacks.isEqual(bucket->key, key))
			return bucket->value;
	}

	return NULL;
}

bool
OSMapTableSetValue(OSMapTable *table, const void *key, const void *value)
{
	unsigned long hash;
	size_t index;
	struct bucket *bucket;

	if (table == NULL || key == NULL)
		return false;

	if (table->entries_n >= (table->buckets_n * 3) / 4) {
		OSMapTableResize(table);
	}

	hash = table->key_callbacks.hash(key);
	index = hash % table->buckets_n;
	bucket = &table->buckets[index];

	if (bucket->key == NULL) {
		table->key_callbacks.retain(key);
		table->value_callbacks.retain(value);
		bucket->key = (void *)key;
		bucket->value = (void *)value;
		table->entries_n++;
		return true;
	}

	if (table->key_callbacks.isEqual(bucket->key, key)) {
		table->key_callbacks.release(bucket->key);
		table->value_callbacks.release(bucket->value);

		bucket->key = (void *)key;
		bucket->value = (void *)value;
		return true;
	}

	for (struct bucket *cur = bucket->next; cur != NULL; cur = cur->next) {
		if (table->key_callbacks.isEqual(cur->key, key)) {
			/* replace */
			table->key_callbacks.release(cur->key);
			table->value_callbacks.release(cur->value);

			table->key_callbacks.retain(key);
			table->value_callbacks.retain(value);

			cur->key = (void *)key;
			cur->value = (void *)value;
			return true;
		}
	}

	struct bucket *new_bucket = kmem_alloc(sizeof(struct bucket));
	if (new_bucket == NULL)
		return false;

	table->key_callbacks.retain(key);
	table->value_callbacks.retain(value);

	new_bucket->key = (void *)key;
	new_bucket->value = (void *)value;
	new_bucket->next = bucket->next;
	bucket->next = new_bucket;
	table->entries_n++;

	return true;
}

bool
OSMapTableRemove(OSMapTable *table, const void *key)
{
	unsigned long hash;
	size_t index;
	struct bucket *bucket;

	if (table == NULL || key == NULL)
		return false;

	hash = table->key_callbacks.hash(key);
	index = hash % table->buckets_n;
	bucket = &table->buckets[index];

	if (bucket->key != NULL &&
	    table->key_callbacks.isEqual(bucket->key, key)) {
		struct bucket *next = bucket->next;

		table->key_callbacks.release(bucket->key);
		table->value_callbacks.release(bucket->value);

		if (next != NULL) {
			bucket->key = next->key;
			bucket->value = next->value;
			bucket->next = next->next;
			kmem_free(next, sizeof(struct bucket));
		} else {
			bucket->key = NULL;
			bucket->value = NULL;
		}

		table->entries_n--;
		return true;
	}

	for (struct bucket *prev = bucket, *cur = bucket->next; cur != NULL;
	     prev = cur, cur = cur->next) {
		if (table->key_callbacks.isEqual(cur->key, key)) {
			/* Release key and value if callbacks exist */
			table->key_callbacks.release(cur->key);
			table->value_callbacks.release(cur->value);

			/* Remove from chain */
			prev->next = cur->next;
			kmem_free(cur, sizeof(struct bucket));
			table->entries_n--;
			return true;
		}
	}

	return false;
}

void
OSMapTableDestroy(OSMapTable *table)
{
	if (table == NULL)
		return;

	for (size_t i = 0; i < table->buckets_n; i++) {
		struct bucket *bucket, *cur, *next;

		bucket = &table->buckets[i];

		if (bucket->key) {
			table->key_callbacks.release(bucket->key);
			table->value_callbacks.release(bucket->value);
		}

		for (cur = bucket->next; cur != NULL; cur = next) {
				next = cur->next;
			table->key_callbacks.release(cur->key);
			table->value_callbacks.release(cur->value);
			kmem_free(cur, sizeof(struct bucket));
		}
	}

	kmem_free(table->buckets, sizeof(struct bucket) * table->buckets_n);
	kmem_free(table, sizeof(struct OSMapTable));
}

size_t
OSMapTableGetCount(const OSMapTable *table)
{
	return table ? table->entries_n : 0;
}
