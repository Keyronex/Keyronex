#include <sys/param.h>
#include <sys/queue.h>

#ifndef _KERNEL
#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define fatal(...) errx(EXIT_FAILURE, __VA_ARGS__)
#define kmem_alloc(SIZE) malloc(SIZE)
#define kmem_strfree(STR) free(STR)
#define kmem_free(PTR, SIZE) free(PTR)
#define kmem_realloc(PTR, OLDSIZE, SIZE) realloc(PTR, SIZE)
#else
#include <kdk/kernel.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>

#define assert(...) kassert(__VA_ARGS__)
#define fatal(...) kfatal(__VA_ARGS__)
#define printf(...) kdprintf(__VA_ARGS__)
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>

#include "dxf/dxf.h"

struct PreIvars {
	int retainCount;
};

#define PRE_IVARS_ALIGN                                            \
	((sizeof(struct PreIvars) + (__BIGGEST_ALIGNMENT__ - 1)) & \
	    ~(__BIGGEST_ALIGNMENT__ - 1))
#define PRE_IVARS ((struct PreIvars *)(void *)((char *)self - PRE_IVARS_ALIGN))

struct dxfpair {
	TAILQ_ENTRY(dxfpair) link;
	char *key;
	dxf_t *value;
};

TAILQ_HEAD(dxf_dict_head, dxfpair);
TAILQ_HEAD(dxf_array_head, dxf);

/* immediately preceded by a PreIvars */
struct dxf {
	dxf_type_t type; /*!< type of object */
	size_t size;	 /*!< for dict, array, bytes */
	union {
		struct dxf_dict_head dict;
		struct dxf_array_head array;
		char *str;
		uint64_t u64;
		int64_t i64;
		void *bytes;
	};
	TAILQ_ENTRY(dxf) link; /*!< linkage in an enclosing array */
};

static dxf_t *
dxf_alloc(dxf_type_t type)
{
	dxf_t *dxf;

	dxf = kmem_alloc(PRE_IVARS_ALIGN + sizeof(dxf_t));

	((struct PreIvars *)dxf)->retainCount = 1;

	dxf = (void *)((char *)dxf + PRE_IVARS_ALIGN);
	dxf->type = type;

	return dxf;
}

dxf_t *
dxf_retain(dxf_t *self)
{
	__atomic_fetch_add(&PRE_IVARS->retainCount, 1, __ATOMIC_RELAXED);
	return self;
}

#define TAILQ_FOREACH_SAFE(var, head, field, next)                             \
	for ((var) = ((head)->tqh_first);                                      \
	     (var) != TAILQ_END(head) && ((next) = TAILQ_NEXT(var, field), 1); \
	     (var) = (next))
#define TAILQ_END(head) (NULL)

dxf_t *
dxf_release(dxf_t *self)
{
	if (__atomic_fetch_sub(&PRE_IVARS->retainCount, 1, __ATOMIC_ACQ_REL) ==
	    1) {
		switch (self->type) {
		case kDXFTypeStr:
#if 0 /* disabled for now because we need the string around if it's a return \
       */
			kmem_strfree(self->str);
#endif
			break;

		case kDXFTypeArray: {
			dxf_t *memb, *tmp;
			TAILQ_FOREACH_SAFE (memb, &self->array, link, tmp) {
				dxf_release(memb);
			}
			break;
		}

		case kDXFTypeDict: {
			struct dxfpair *pair, *tmp;
			TAILQ_FOREACH_SAFE (pair, &self->dict, link, tmp) {
				dxf_release(pair->value);
				kmem_strfree(pair->key);
				kmem_free(pair, sizeof(*pair));
			}
			break;
		}

		case kDXFTypeBytes: {
			kmem_free(self->bytes, self->size);
		}

		default:
			break;
		}
		kmem_free(PRE_IVARS, PRE_IVARS_ALIGN + sizeof(dxf_t));
	}
	return NULL;
}

dxf_type_t
dxf_type(dxf_t *self)
{
	return self->type;
}

dxf_t *
dxf_create_null(void)
{
	static dxf_t *dxf_null = NULL;

	if (dxf_null == NULL) {
		dxf_null = dxf_alloc(kDXFTypeStr);
		dxf_retain(dxf_null);
	}

	return dxf_null;
}

dxf_t *
dxf_create_dict(void)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeDict);
	TAILQ_INIT(&dxf->dict);
	return dxf;
}

dxf_t *
dxf_create_array(void)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeArray);
	TAILQ_INIT(&dxf->array);
	return dxf;
}

dxf_t *
dxf_movein_str(char *value)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeStr);
	dxf->str = value;
	return dxf;
}

dxf_t *
dxf_create_str(const char *value)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeStr);
	dxf->str = strdup(value);
	return dxf;
}

dxf_t *
dxf_create_i64(uint64_t value)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeI64);
	dxf->i64 = value;
	return dxf;
}

dxf_t *
dxf_create_u64(uint64_t value)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeU64);
	dxf->u64 = value;
	return dxf;
}

dxf_t *
dxf_create_bytes(size_t size)
{
	dxf_t *dxf = dxf_alloc(kDXFTypeBytes);
	dxf->bytes = kmem_alloc(size);
	dxf->size = size;
	return dxf;
}

static dxf_t *
do_array_get_value(dxf_t *dxf, size_t index, bool move)
{
	dxf_t *it, *result = NULL;
	size_t i = 0;

	TAILQ_FOREACH (it, &dxf->array, link) {
		if (i++ == index) {
			result = it;
			break;
		}
	}

	if (!result)
		return NULL;

	assert(!move);

	return result;
}

dxf_t *
dxf_array_get_value(dxf_t *dxf, size_t index)
{
	return do_array_get_value(dxf, index, false);
}

int
dxf_array_append_value(dxf_t *dxf, dxf_t *value)
{
	assert(dxf->type == kDXFTypeArray);
	TAILQ_INSERT_TAIL(&dxf->array, dxf_retain(value), link);
	return 0;
}

int
dxf_array_movein_value(dxf_t *dxf, dxf_t *value)
{
	assert(dxf->type == kDXFTypeArray);
	TAILQ_INSERT_TAIL(&dxf->array, value, link);
	return 0;
}

void *
dxf_bytes_get_data_ptr(dxf_t *dxf)
{
	assert(dxf->type == kDXFTypeBytes);
	return dxf->bytes;
}

static dxf_t *
do_dict_get_value(dxf_t *dxf, const char *key, bool move)
{
	struct dxfpair *it, *pair = NULL;

	TAILQ_FOREACH (it, &dxf->dict, link) {
		if (strcmp(it->key, key) == 0) {
			pair = it;
			break;
		}
	}

	if (!pair)
		return NULL;

	assert(!move);

	return pair->value;
}

dxf_t *
dxf_dict_get_value(dxf_t *dxf, const char *key)
{
	return do_dict_get_value(dxf, key, false);
}

const char *
dxf_dict_get_string(dxf_t *dxf, const char *key)
{
	dxf_t *val = dxf_dict_get_value(dxf, key);
	assert(val != NULL && val->type == kDXFTypeStr);
	return val->str;
}

uint64_t
dxf_dict_get_u64(dxf_t *dxf, const char *key)
{
	dxf_t *val = dxf_dict_get_value(dxf, key);
	assert(val != NULL && val->type == kDXFTypeU64);
	return val->u64;
}

static int
do_dict_set_value(dxf_t *dxf, const char *key, dxf_t *val, bool move)
{
	struct dxfpair *it, *pair = NULL;

	if (val == NULL)
		return -ENOMEM;

	TAILQ_FOREACH (it, &dxf->dict, link) {
		if (strcmp(it->key, key) == 0) {
			pair = it;
			break;
		}
	}

	if (!pair) {
		pair = kmem_alloc(sizeof(struct dxfpair));
		pair->key = strdup(key);
		TAILQ_INSERT_TAIL(&dxf->dict, pair, link);
	} else {
		dxf_release(pair->value);
	}

	pair->value = move ? val : dxf_retain(val);

	return 0;
}

int
dxf_dict_movein_value(dxf_t *dxf, const char *key, dxf_t *val)
{
	return do_dict_set_value(dxf, key, val, true);
}

int
dxf_dict_set_value(dxf_t *dxf, const char *key, dxf_t *val)
{
	return do_dict_set_value(dxf, key, val, false);
}

int
dxf_dict_set_string(dxf_t *dxf, const char *key, const char *value)
{
	assert(dxf->type == kDXFTypeDict);
	return dxf_dict_movein_value(dxf, key, dxf_create_str(value));
}

int
dxf_dict_set_u64(dxf_t *dxf, const char *key, uint64_t value)
{
	assert(dxf->type == kDXFTypeDict);
	return dxf_dict_movein_value(dxf, key, dxf_create_u64(value));
}

int
dxf_dict_set_null(dxf_t *dxf, const char *key)
{
	assert(dxf->type == kDXFTypeDict);
	return dxf_dict_movein_value(dxf, key, dxf_create_null());
}

int64_t
dxf_i64_get_value(dxf_t *dxf)
{
	assert(dxf->type == kDXFTypeI64);
	return dxf->i64;
}

const char *
dxf_string_get_string_ptr(dxf_t *dxf)
{
	assert(dxf->type == kDXFTypeStr);
	return dxf->str;
}

uint64_t
dxf_u64_get_value(dxf_t *dxf)
{
	assert(dxf->type == kDXFTypeU64);
	return dxf->u64;
}

const char *
dxf_type_to_string(dxf_type_t type)
{
	switch (type) {
	case kDXFTypeDict:
		return "dict";
	case kDXFTypeStr:
		return "string";
	case kDXFTypeArray:
		return "array";
	case kDXFTypeI64:
		return "i64";
	case kDXFTypeU64:
		return "u64";
	case kDXFTypeNull:
		return "null";
	default:
		return "????";
	}
}

struct pack_state {
	uint8_t *buf;
	size_t cursor;
	size_t size;
};

enum pack_type {
	kZero,
	kDictBegin,
	kDictEnd,
	kArrayBegin,
	kArrayEnd,
	kString,
	kU64,
	kI64,
	kBytes,
	kNull,
};

#define TRY(...)         \
	r = __VA_ARGS__; \
	if (r < 0)       \
	return r

static int
emit_u8(struct pack_state *state, uint8_t val)
{
	if (state->cursor + 1 > state->size) {
		void *newbuf;
		size_t newsize;
		newsize = state->size / 2 * 3;
		newbuf = kmem_realloc(state->buf, state->size, newsize);
		if (!newbuf)
			return -ENOMEM;
		state->buf = newbuf;
		state->size = newsize;
	}

	state->buf[state->cursor++] = val;

	return 0;
}

static int
emit_bytes(struct pack_state *state, const void *bytes, size_t len)
{
	if (state->cursor + len > state->size) {
		void *newbuf;
		size_t newsize = state->size / 2 * 3;
		newsize = MAX(newsize, state->size + len / 2 * 3);
		newbuf = kmem_realloc(state->buf, state->size, newsize);
		if (!newbuf)
			return -ENOMEM;
		state->buf = newbuf;
		state->size = newsize;
	}

	memcpy(&state->buf[state->cursor], bytes, len);
	state->cursor += len;

	return 0;
}

static int
emit_string(struct pack_state *state, const char *str)
{
	size_t len = strlen(str);
	int r;

	if (state->cursor + len + 2 > state->size) {
		void *newbuf;
		size_t newsize = state->size / 2 * 3;
		newsize = MAX(newsize, state->size + (len + 2) / 2 * 3);
		newbuf = kmem_realloc(state->buf, state->size, newsize);
		if (!newbuf)
			return -ENOMEM;
		state->buf = newbuf;
		state->size = newsize;
	}

	TRY(emit_u8(state, kString));
	TRY(emit_u8(state, len));
	return emit_bytes(state, str, len);
}

static int
do_pack(dxf_t *dxf, struct pack_state *state)
{
	int r;

	switch (dxf->type) {
	case kDXFTypeDict: {
		struct dxfpair *pair;

		TRY(emit_u8(state, kDictBegin));

		TAILQ_FOREACH (pair, &dxf->dict, link) {
			TRY(emit_string(state, pair->key));
			TRY(do_pack(pair->value, state));
		}
		TRY(emit_u8(state, kDictEnd));

		return 0;
	}

	case kDXFTypeStr:
		return emit_string(state, dxf->str);

	case kDXFTypeArray: {
		struct dxf *el;
		TRY(emit_u8(state, kArrayBegin));
		TAILQ_FOREACH (el, &dxf->array, link) {
			TRY(do_pack(el, state));
		}
		return emit_u8(state, kArrayEnd);
	}

	case kDXFTypeI64:
		TRY(emit_u8(state, kI64));
		return emit_bytes(state, &dxf->i64, sizeof(int64_t));

	case kDXFTypeU64:
		TRY(emit_u8(state, kU64));
		return emit_bytes(state, &dxf->i64, sizeof(uint64_t));

	case kDXFTypeBytes:
		TRY(emit_u8(state, kBytes));
		TRY(emit_bytes(state, &dxf->size, sizeof(uint64_t)));
		return emit_bytes(state, dxf->bytes, dxf->size);

	case kDXFTypeNull:
		return emit_u8(state, kNull);

	default:
		fatal("Bad DXF type %d\n", dxf->type);
	}
}

ssize_t
dxf_pack(dxf_t *dxf, void **out, int **fds_out)
{
	ssize_t size = 0;
	struct pack_state state;

	state.buf = kmem_alloc(64);
	state.size = 64;
	state.cursor = 0;

	size = do_pack(dxf, &state);
	if (size < 0) {
		kmem_free(state.buf, state.size);
		return size;
	}

	*out = state.buf;
	return state.cursor;
}

static uint8_t
peek_u8(struct pack_state *state, uint8_t *out)
{
	assert(state->cursor < state->size);
	*out = state->buf[state->cursor];
	return 0;
}

static uint8_t
get_u8(struct pack_state *state, uint8_t *out)
{
	assert(state->cursor < state->size);
	*out = state->buf[state->cursor++];
	return 0;
}

static int
get_bytes(struct pack_state *state, void *bytes, size_t len)
{
	assert(state->cursor + len <= state->size);
	memcpy(bytes, &state->buf[state->cursor], len);
	state->cursor += len;
	return 0;
}

static int
do_unpack(struct pack_state *state, dxf_t **out)
{
	uint8_t type;
	dxf_t *result;
	int r;

	r = get_u8(state, &type);

	switch ((enum pack_type)type) {
	case kDictBegin: {
		result = dxf_create_dict();
		while (true) {
			dxf_t *key = NULL, *val = NULL;

			r = peek_u8(state, &type);
			if (r != 0)
				goto faila;
			if (type == kDictEnd)
				break;

			r = do_unpack(state, &key);
			if (r != 0)
				goto faila;

			r = do_unpack(state, &val);
			if (r != 0)
				goto faila;

			if (key->type != kDXFTypeStr)
				goto faila;

			dxf_dict_movein_value(result, key->str, val);
			dxf_release(key);

			continue;

		faila:
			dxf_release(key);
			dxf_release(val);
			dxf_release(result);
			return r;
		}

		/* n.b. type is not read after this */
		if ((r = get_u8(state, &type)) < 0) {
			dxf_release(result);
			return r;
		}

		break;
	}

	case kArrayBegin: {
		result = dxf_create_array();

		while (true) {
			dxf_t *val = NULL;

			r = peek_u8(state, &type);
			if (r != 0)
				goto failb;
			if (type == kArrayEnd)
				break;

			r = do_unpack(state, &val);
			if (r != 0)
				goto failb;

			r = dxf_array_movein_value(result, val);
			if (r != 0)
				goto failb;

			continue;

		failb:
			dxf_release(val);
			dxf_release(result);
			return r;
		}
		/* n.b. type is not read after this */
		if ((r = get_u8(state, &type)) < 0) {
			dxf_release(result);
			return r;
		}
		break;
	}

	case kString: {
		uint8_t len;
		char *data;

		r = get_u8(state, &len);
		if (r < 0)
			return r;

		data = kmem_alloc(len + 1);
		data[len] = '\0';

		r = get_bytes(state, data, len);
		if (r < 0) {
			kmem_free(data, len + 1);
			return r;
		}

		result = dxf_movein_str(data);
		break;
	}

	case kI64: {
		int64_t val;
		r = get_bytes(state, &val, sizeof(int64_t));
		if (r < 0)
			return r;

		result = dxf_create_i64(val);
		break;
	}

	case kU64: {
		uint64_t val;

		r = get_bytes(state, &val, sizeof(uint64_t));
		if (r < 0)
			return r;

		result = dxf_create_u64(val);
		break;
	}

	case kBytes: {
		uint64_t size;

		r = get_bytes(state, &size, sizeof(uint64_t));
		if (r < 0)
			return r;

		result = dxf_create_bytes(size);

		r = get_bytes(state, dxf_bytes_get_data_ptr(result), size);
		if (r < 0) {
			dxf_release(result);
			return r;
		}

		break;
	}

	case kNull:
		result = dxf_create_null();
		break;

	default:
		return -EINVAL;
	}

	*out = result;
	return 0;
}

dxf_t *
dxf_unpack(void *buf, size_t size, int *fds)
{
	struct pack_state state;
	dxf_t *dxf;
	int r;

	state.buf = buf;
	state.size = size;
	state.cursor = 0;

	r = do_unpack(&state, &dxf);
	if (r < 0)
		return NULL;

	return dxf;
}

static void
do_dump(dxf_t *dxf)
{
	switch (dxf->type) {
	case kDXFTypeDict: {
		struct dxfpair *pair;
		bool is_first = true;

		printf("{ ");
		TAILQ_FOREACH (pair, &dxf->dict, link) {
			if (is_first)
				is_first = false;
			else
				printf(", ");
			printf("\"%s\": ", pair->key);
			do_dump(pair->value);
		}
		printf(" }");
		return;
	}
	case kDXFTypeStr:
		printf("\"%s\"", dxf->str);
		return;
	case kDXFTypeArray: {
		struct dxf *el;
		bool is_first = true;

		printf("[ ");
		TAILQ_FOREACH (el, &dxf->array, link) {
			if (is_first)
				is_first = false;
			else
				printf(", ");
			do_dump(el);
		}
		printf(" ]");
		return;
	}
	case kDXFTypeI64:
		printf("%" PRIi64, dxf->i64);
		return;
	case kDXFTypeU64:
		printf("%" PRIu64, dxf->u64);
		return;
	case kDXFTypeBytes: {
		bool is_first = true;

		printf("$[");
		for (size_t i = 0; i < dxf->size; i++) {
			if (is_first)
				is_first = false;
			else
				printf(", ");
			printf("%x",
			    ((uint8_t *)dxf_bytes_get_data_ptr(dxf))[i]);
		}
		printf("]$");
		return;
	}
	case kDXFTypeNull:
		printf("null");
		return;
	default:
		fatal("Bad DXF type %d\n", dxf->type);
	}
}

void
dxf_dump(dxf_t *dxf)
{
	do_dump(dxf);
	printf("\n");
}
