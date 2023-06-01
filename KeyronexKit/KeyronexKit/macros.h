#ifndef _MACROS_H
#define _MACROS_H

#ifdef _KERNEL
#include <kdk/kmem.h>
#else
#include <stdlib.h>

#define fatal(...) errx(EXIT_FAILURE, __VA_ARGS__)
#define kmem_alloc(SIZE) malloc(SIZE)
#define kmem_free(PTR, SIZE) free(PTR)
#define kmem_realloc(PTR, OLDSIZE, SIZE) realloc(PTR, SIZE)
#endif

#endif /* _MACROS_H */
