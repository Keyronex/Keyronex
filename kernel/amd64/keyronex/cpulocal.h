/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpulocal.h
 * @brief AMD64 CPU-local data handling
 */

#ifndef ECX_AMD64_KERN_CPULOCAL_H
#define ECX_AMD64_KERN_CPULOCAL_H

#include <stdint.h>

struct tss;

struct karch_cpu_data {
	uint32_t lapic_id;
#define arch_cpu_id lapic_id
	struct tss *tss;
};


#define CPU_LOCAL_OFFSET(FIELD) __builtin_offsetof(struct kcpu_data, FIELD)

#define CPU_LOCAL_LOAD(FIELD) ({ 				\
	volatile __seg_gs struct kcpu_data *_local = 0;	\
	_local->FIELD;						\
})

#define CPU_LOCAL_STORE(FIELD, VALUE) ({			\
	volatile __seg_gs struct kcpu_data *_local = 0;	\
	_local->FIELD = VALUE;					\
})

#define CPU_LOCAL_GET() CPU_LOCAL_LOAD(self)

/* note non-volatile */
#define CPU_LOCAL_ADDROF(FIELD) ({ &((CPU_LOCAL_GET())->FIELD); })

#endif /* ECX_AMD64_KERN_CPULOCAL_H */
