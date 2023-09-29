#ifndef KRX_DEV_FSLIB_H
#define KRX_DEV_FSLIB_H

#include <dirent.h>

#include "kdk/libkern.h"
#include "kdk/nanokern.h"

#define DIRENT_RECLEN(NAMELEN) \
	ROUNDUP(offsetof(struct dirent, d_name[0]) + 1 + NAMELEN, 8)

static uint32_t
strhash32(const char *str, size_t len)
{
	static const uint32_t fnv_prime = 0x811C9DC5, fnv_basis = 0x01000193;
	uint32_t hash = fnv_basis;

	for (size_t i = 0; i < len; i++) {
		hash *= fnv_prime;
		hash ^= str[i];
	}

	return hash >> 10 | ((uint32_t)len << 22);
}

/*!
 * 0 = success
 * -1 = no space left in dirent buffer
 */
static inline int
fill_dirent(void *buf, size_t buf_len, void **state, size_t offset,
    const char *name, uint64_t ino, unsigned char type)
{
	struct dirent *dent = *state;
	size_t reclen = DIRENT_RECLEN(strlen(name));

	if (((char *)*state + reclen) >= ((char *)buf + buf_len))
		return -1;

	dent->d_off = offset;
	dent->d_ino = ino;
	dent->d_type = type;
	strcpy(dent->d_name, name);
	dent->d_reclen = DIRENT_RECLEN(strlen(name));

	return 0;
}


#endif /* KRX_DEV_FSLIB_H */
