/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Jul 28 2024.
 */

#ifndef KRX_AARCH64_VIRT_GIC_H
#define KRX_AARCH64_VIRT_GIC_H

#include <stdbool.h>
#include <stdint.h>

uint32_t gengic_acknowledge(void);
void gengic_eoi(uint32_t intr);

void gengic_dist_setedge(uint32_t gsi, bool edge);
void gengic_dist_settarget(uint32_t gsi, uint64_t target);
void gengic_dist_setenabled(uint32_t gsi);

#endif /* KRX_AARCH64_VIRT_GIC_H */
