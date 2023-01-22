#ifndef LIBCCOMPAT_H_
#define LIBCCOMPAT_H_

#include <libkern/libkern.h>
#include <kern/kmem_liballoc.h>
#include <string.h>

#define fflush(...)
#define printf(...) kprintf(__VA_ARGS__)
#define fprintf(file, ...) kprintf(__VA_ARGS__)
#define vfprintf(file, ...) kvpprintf(__VA_ARGS__)

#define assert(...) nk_assert(__VA_ARGS__)
#define abort() nk_fatal("abort!")
#define fatal(...) nk_fatal(__VA_ARGS__)

#define malloc liballoc_kmalloc
#define calloc liballoc_kcalloc
#define free liballoc_kfree
#define realloc liballoc_krealloc

#endif /* LIBCCOMPAT_H_ */
