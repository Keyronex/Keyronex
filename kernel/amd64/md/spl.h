/*
 * System Priority Level
 */

#ifndef ARCH_SPL_H_
#define ARCH_SPL_H_

#include <stdint.h>

typedef enum {
	kSPLHigh = 15, /* blocks all */
	kSPLDispatch = 2,
	kSPL0 = 0, /* blocks none */
} ipl_t;

void nkx_spl_lowered(ipl_t from, ipl_t to);

/*! get current IPL */
static inline ipl_t
splget()
{
	uint64_t spl;
	asm volatile("mov %%cr8, %0\n" : "=r"(spl));
	return spl; /* interrupt enable flag */
}

/*! set IPL to \p spl and return the old IPL */
static inline ipl_t
splx(ipl_t spl)
{
	uint64_t spli = spl;
	uint64_t oldspl = splget();
	asm volatile("movq %0,%%cr8" ::"r"(spli) : "memory");
	if (oldspl > spl) {
		nkx_spl_lowered(oldspl, spl);
	}
	return oldspl;
}

/*! raise IPL to \p spl */
ipl_t splraise(ipl_t spl);

#define splhigh() splraise(kSPLHigh)
#define spldispatch() splraise(kSPLDispatch)

/*! lower to IPL 0; use is sus, use splx instead */
static inline ipl_t
spl0()
{
	return splx(kSPL0);
}

#endif /* ARCH_SPL_H_ */
