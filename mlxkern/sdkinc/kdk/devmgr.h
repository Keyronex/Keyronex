/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 21 2023.
 */

#ifndef MLX_KDK_DEVMGR_H
#define MLX_KDK_DEVMGR_H

#include <bsdqueue/queue.h>
#include <stdint.h>

#include "kdk/objhdr.h"
#include "kdk/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t io_bsize_t;
typedef int64_t io_boff_t;
typedef uint64_t io_size_t;
typedef int64_t io_off_t;

/*!
 * I/O packet function.
 */
typedef enum iop_function {
	kIOPTypeRead,
} iop_function_t;

/*! For kIOPTypeRead. */
struct iop_stack_data_read {
	/*! Number of bytes to read. */
	io_size_t bytes;
	/*! Offset in file at which to read. */
	io_off_t offset;
};

/*!
 * An entry in an I/O packet stack.
 */
typedef struct iop_stack_entry {
	/*! Which function is being carried out? */
	iop_function_t function;
	/*! MDL for principal input/out */
	vm_mdl_t *mdl;
	/*! Union of different types. */
	union {
		struct iop_stack_data_read read;
	};
} iop_stack_entry_t;

/*!
 * In which direction is the packet travelling?
 *
 * Generally down is what happens first, as the IOP is processed by elements of
 * the stack. Then it moves upwards again.
 */
typedef enum iop_direction {
	/*! IOP travelling down the device stack. */
	kIOPDown,
	/*! IOP travelling up the device stack. */
	kIOPUp,
} iop_direction_t;

/*!
 * I/O packet
 */
typedef struct iop {
	/*! Which direction is it currently travelling? */
	iop_direction_t direction;
	/*! How many items are on the stack? */
	uint8_t stack_count;
	/*! Current stack entry. Higher levels have lower numbers. */
	uint8_t stack_current;
	/*!
	 * The stack, containing at least #stack_count entries, ordered starting
	 * with the highest level first.
	 */
	iop_stack_entry_t stack[0];
} iop_t;

/*!
 * A device object.
 */
typedef struct device {
	object_header_t objhdr;

	TAILQ_HEAD(, device) consumers;
	TAILQ_ENTRY(device) consumers_link;
	struct device *provider;
	uint8_t stack_depth;

	/*! Dispatch an I/O packet. */
	mlx_status_t (*dispatch)(struct device *dev, iop_t *iop);

	/*! driver-specific context */
	// void *context;
} device_t;

#if 0
/*!
 * A driver object.
 */
typedef struct driver {

} driver_t;
#endif

/*!
 * Create a device. object.
 */
// int dev_create(d)

/*!
 * Attach a device object as consumer of another device object.
 */
void dev_attach(device_t *consumer, device_t *provider);

/*!
 * @brief Return a pointer to the current stack entry of an IOP.
 */
inline iop_stack_entry_t *
iop_stack_current(iop_t *iop)
{
	return &iop->stack[iop->stack_current];
}

#ifdef __cplusplus
}
#endif

#endif /* MLX_KDK_DEVMGR_H */
