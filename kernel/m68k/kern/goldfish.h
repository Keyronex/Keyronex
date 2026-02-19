/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file goldfish.h
 * @brief Goldfish definitions.
 */

#ifndef ECX_KERN_GOLDFISH_H
#define ECX_KERN_GOLDFISH_H

#include <sys/k_intr.h>
#include <sys/pcb.h>

#include <stdint.h>
#include <stdbool.h>

#define PIC_IRQ(PIC, IRQ) (((PIC)-1) * 32 + ((IRQ)-1))

void gfpic_dispatch(unsigned int pic_num, karch_trapframe_t *frame);
void gfpic_unmask_irq(unsigned int vector);
void gfpic_handle_irq(unsigned int vector, bool (*handler)(void *), void *arg);

void gfrtc_init(void);
uint64_t gfrtc_get_time(void);
void gfrtc_oneshot(uint64_t ns);

void gftty_init(void);

#endif /* ECX_KERN_GOLDFISH_H */
