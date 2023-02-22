/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 21 2023.
 */

#ifndef MLX_KDK_DEVMGR_H
#define MLX_KDK_DEVMGR_H

#include <bsdqueue/queue.h>
#include <stdint.h>

#include "kdk/objhdr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum irp_function {
	kIRPTypeRead,
} irp_function_t;

typedef struct irp_stack_entry {
	irp_function_t function;
} irp_stack_entry_t;

typedef struct irp {
	/*! How many items are on the stack? */
	uint8_t stack_count;
	/*!
	 * The stack, containing at least #stack_count entries, ordered starting
	 * with the highest level first.
	 */
	irp_stack_entry_t stack[0];
} irp_t;

/*!
 * A device object.
 */
typedef struct device {
	object_header_t objhdr;

	TAILQ_HEAD(, device) consumers;
	TAILQ_ENTRY(device) consumers_link;
	struct device * provider;
	uint8_t stack_depth;

	/*! driver-specific context */
	//void *context;
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
//int dev_create(d)

/*!
 * Attach a device object as consumer of another device object.
 */
void dev_attach(device_t *consumer, device_t *provider);

#ifdef __cplusplus
}
#endif

#endif /* MLX_KDK_DEVMGR_H */
