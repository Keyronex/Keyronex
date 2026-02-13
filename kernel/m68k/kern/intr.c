/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file ipl.c
 * @brief m68k interrupt handling.
 */

#include <keyronex/cpu.h>
#include <keyronex/cpulocal.h>
#include <keyronex/dlog.h>
#include <keyronex/pcb.h>
#include "goldfish.h"

#define M68K_SR_IPL_MASK 0x0700u

void kep_dispatch_softints(ipl_t newipl);

static inline uint16_t
m68k_sr_read(void)
{
	uint16_t sr;
	asm volatile("move.w %%sr, %0" : "=d"(sr)::"memory");
	return sr;
}

static inline void
m68k_sr_write(uint16_t sr)
{
	asm volatile("move.w %0, %%sr" ::"d"(sr) : "cc", "memory");
}

static inline uint16_t
ipl_to_sr_part(ipl_t ipl)
{
	if (ipl <= IPL_M68K_0)
		return 0;
	else
		return ((uint16_t)ipl - IPL_M68K_0) << 8;
}

bool
ke_arch_disable(void)
{
	uint16_t sr = m68k_sr_read();

	if ((sr & M68K_SR_IPL_MASK) != M68K_SR_IPL_MASK) {
		m68k_sr_write((uint16_t)(sr | M68K_SR_IPL_MASK));
		return true;
	}

	return false;
}

void
ke_arch_enable(bool token)
{
	uint16_t sr, newpart;

	if (!token)
		return;

	sr = m68k_sr_read();
	newpart = ipl_to_sr_part(ke_ipl());

	sr = (uint16_t)((sr & ~M68K_SR_IPL_MASK) | newpart);
	m68k_sr_write(sr);
}

ipl_t
ke_ipl(void)
{
	return CPU_LOCAL_LOAD(ipl);
}

ipl_t
splraise(ipl_t newipl)
{
	int tok = ke_arch_disable();
	ipl_t oldipl = CPU_LOCAL_LOAD(ipl);

	kassert(newipl >= oldipl, "splraise: lowering ipl");
	CPU_LOCAL_STORE(ipl, newipl);

	ke_arch_enable(tok);
	return oldipl;
}

static inline uint32_t
pending_soft_ints(void)
{
	return atomic_load_explicit(&ke_bsp_cpu_data.pending_soft_ints,
	    memory_order_relaxed);
}

void
splx(ipl_t ipl)
{
	int tok = ke_arch_disable();

	ipl_t previpl = CPU_LOCAL_LOAD(ipl);
	CPU_LOCAL_STORE(ipl, ipl);

	kassert(previpl >= ipl, "splx: to higher IPL");

	ke_arch_enable(tok);

	if ((pending_soft_ints() >> (uint32_t)ipl) != 0UL)
		kep_dispatch_softints(ipl);
}

void
kep_arch_set_vbase(void)
{
	static void *ivt[256];
	extern void *asm_trap;
	for (int i = 0; i < 256; i++)
		ivt[i] = &asm_trap;
	asm volatile("move.c %0, %%vbr" ::"r"(ivt));
	asm volatile("andi %0, %%sr" ::"i"(~(7 << 8)) : "memory");
}

/*
 * The principle: in asm_trap, we immediately do `ori.w #0x0700, %sr`, making
 * hardware IPL 7.
 *
 * Then here in c_trap, we identify the IPL we should really be at by consulting
 * the vector offset. We call ke_arch_enable() to reflect that IPL in %sr.
 *
 * Finally, we can call splx() to reinstate the old IPL.
 */
void
c_trap(karch_trapframe_t *frame)
{
	ipl_t oldhwipl = IPL_M68K_0 + ((frame->sr & M68K_SR_IPL_MASK) >> 8);
	ipl_t oldipl = MIN2(oldhwipl, CPU_LOCAL_LOAD(ipl));

	switch (frame->vector_offset) {
	case 0x64: /* level 1 autovector */
	case 0x68:
	case 0x6c:
	case 0x70:
	case 0x74:
	case 0x78:
	case 0x7c:
		CPU_LOCAL_STORE(ipl,
		    IPL_M68K_1 + (frame->vector_offset - 0x64) / 4);
		ke_arch_enable(true);

		gfpic_dispatch((frame->vector_offset - 0x64) / 4, frame);
		break;

	default:
		CPU_LOCAL_STORE(ipl, IPL_HIGH);
		kfatal("unhandled trap, vector 0x%x", frame->vector_offset);
	}

	splx(oldipl);
}
