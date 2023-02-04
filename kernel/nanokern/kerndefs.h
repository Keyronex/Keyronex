
/*!
 * annotations, types, definitions and suchlike
 */

#ifndef KERN_KERNDEFS_H_
#define KERN_KERNDEFS_H_

#include <stdint.h>

/*! parameter is an input */
#define kx_in
/*! parameter is an output */
#define kx_out
/*! parameter is nullable */
#define kx_nullable

/* calling function requires this lock to be held */
#define LOCK_REQUIRES(lock)

/* calling function requires this lock to be held, and releases with lock unheld */
#define LOCK_RELEASE(lock)

#define NS_PER_S 1000000000

#define KERN_HZ 100

#define elementsof(x)	( sizeof(x) / sizeof(x[0]) )

typedef uint64_t nanosec_t;

typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;
typedef uintptr_t voff_t;
typedef uintptr_t poff_t;
typedef uintptr_t pgoff_t;

#endif /* KERN_KERNDEFS_H_ */
