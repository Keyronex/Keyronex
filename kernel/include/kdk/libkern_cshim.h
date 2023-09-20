#ifndef KRX_KDK_LIBKERN_CSHIM_H
#define KRX_KDK_LIBKERN_CSHIM_H

#include <kdk/nanokern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>

#define fflush(...)
#define printf(...) kprintf(__VA_ARGS__)
#define fprintf(file, ...) kprintf(__VA_ARGS__)
#define vfprintf(file, ...) pac_vpprintf(__VA_ARGS__)

#define assert(...) kassert(__VA_ARGS__)
#define abort() kfatal("abort!")
#define fatal(...) kfatal(__VA_ARGS__)

#define malloc kmem_malloc
#define calloc kmem_calloc
#define free kmem_mfree
#define realloc kmem_mrealloc

typedef int32_t ssize_t;

struct timeval {
	int64_t tv_sec;
	int64_t tv_usec;
};

extern int stdin, stdout, stderr;

#endif /* KRX_KDK_LIBKERN_CSHIM_H */
