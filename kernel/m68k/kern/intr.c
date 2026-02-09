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

	if ((pending_soft_ints() >> (uint64_t)ipl) != 0ULL)
		kep_dispatch_softints(ipl);
}

/*
 * principle:
 * - first instruction of ASM trap handler should clear IPL
 * - C trap handler will, based on autovector number, set IPL, deriving previous
 *     IPL from that saved in trap frame.
 * - can lower IPL again at end
 */
