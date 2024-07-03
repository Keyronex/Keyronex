#ifndef KRX_AARCH64_CPU_H
#define KRX_AARCH64_CPU_H

#include <stdint.h>

struct __attribute__((packed)) id_aa64pfr0_el1 {
	uint64_t el0: 4, el1: 4, el2: 4, el4: 4, fp: 4, advsimd: 4, gic: 4, ras: 4, sve:4 , sel2: 4, mpam:4, amu: 4, dit: 4, res0: 4, csv2: 4, csv3: 4;
	/*
	uint64_t csv3 : 4, csv2 : 4, res0 : 4, dit : 4, amu : 4, mpam : 4,
	    sel2 : 4, sve : 4, ras : 4, gic : 4, avdvsimd : 4, fp : 4, el3 : 4,
	    el2 : 4, el1 : 4, el0 : 4;
	*/
};

static inline struct id_aa64pfr0_el1
read_id_aa64pfr0_el1(void)
{
	struct id_aa64pfr0_el1 res;
	asm("mrs %0, id_aa64pfr0_el1" : "=r"(res));
	return res;
}

#endif /* KRX_AARCH64_CPU_H */
