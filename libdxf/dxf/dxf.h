#ifndef _DXF_H
#define _DXF_H

#include <sys/types.h>

#include <stdint.h>

typedef enum dxf_type {
	kDXFTypeDict,
	kDXFTypeArray,
	kDXFTypeStr,
	kDXFTypeU64,
	kDXFTypeI64,
	kDXFTypeNull,
} dxf_type_t;
typedef struct dxf dxf_t;
typedef struct dxfpair dxfpair_t;

dxf_t *dxf_retain(dxf_t *self);
dxf_t *dxf_release(dxf_t *self);

dxf_type_t dxf_type(dxf_t *self);

dxf_t *dxf_create_null(void);
dxf_t *dxf_create_array(void);
dxf_t *dxf_create_dict(void);
dxf_t *dxf_create_str(const char *value);
dxf_t *dxf_create_i64(uint64_t value);
dxf_t *dxf_create_u64(uint64_t value);

dxf_t *dxf_array_get_value(dxf_t *dxf, size_t index);

int dxf_array_append_value(dxf_t *dxf, dxf_t *value);

dxf_t *dxf_dict_get_value(dxf_t *dxf, const char *key);
const char *dxf_dict_get_string(dxf_t *dxf, const char *key);
uint64_t dxf_dict_get_u64(dxf_t *dxf, const char *key);

int dxf_dict_set_value(dxf_t *dxf, const char *key, dxf_t *val);
int dxf_dict_set_string(dxf_t *dxf, const char *key, const char *value);
int dxf_dict_set_u64(dxf_t *dxf, const char *key, uint64_t value);
int dxf_dict_set_null(dxf_t *dxf, const char *key);

int64_t dxf_i64_get_value(dxf_t *dxf);

const char *dxf_string_get_string_ptr(dxf_t *dxf);

uint64_t dxf_u64_get_value(dxf_t *dxf);

ssize_t dxf_pack(dxf_t *dxf, void **out, int **fds_out);
dxf_t *dxf_unpack(void *buf, size_t size, int *fds);

void dxf_dump(dxf_t *dxf);

#endif /* _DXF_H */
