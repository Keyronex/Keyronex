/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 21 2023.
 */

#ifndef MLX_DEVMGR_DEVMGR_H
#define MLX_DEVMGR_DEVMGR_H

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
	irp_stack_entry_t stack[0];
} irp_t;

#ifdef __cplusplus
}
#endif

#endif /* MLX_DEVMGR_DEVMGR_H */
