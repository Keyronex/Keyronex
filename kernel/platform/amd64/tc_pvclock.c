/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Feb 15 2025.
 */
/*!
 * @file tc_pvclock.c
 * @brief Paravirtual Clock (from KVM) based timecounter.
 */

#include <kdk/amd64/cpuid.h>
#include <kdk/amd64/regs.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/vm.h>

#include "kern/ki.h"

#define KVM_FEATURE_CLOCKSOURCE2 3
#define KVM_FEATURE_CLOCKSOURCE_STABLE_BIT 24

#define MSR_KVM_SYSTEM_TIME_NEW 0x4b564d01

struct pvclock_vcpu_time_info {
	uint32_t version;
	uint32_t pad0;
	uint64_t tsc_timestamp;
	uint64_t system_time;
	uint32_t tsc_to_system_mul;
	int8_t tsc_shift;
	uint8_t flags;
	uint8_t pad[2];
};

static nanosecs_t pvclock_get_nanos(void);

static volatile struct pvclock_vcpu_time_info *vcpu_time_info;
static bool using_pvclock = false;

static inline uint64_t
rdtsc_serialised(void)
{
	uint64_t ret;
	asm volatile("lfence; rdtsc" : "=A"(ret) : : "memory");
	return ret;
}

void
pvclock_init(void)
{
	struct cpuid_ret data;
	vm_page_t *page;

	data = cpuid(0x40000000, 0);
	if (!(data.reg[1] == 0x4b4d564b) && (data.reg[2] == 0x564b4d56) &&
	    (data.reg[3] == 0x4d))
		return; /* not running under KVM! */

	data = cpuid(0x40000001, 0);
	if (!(data.reg[0] & (1 << KVM_FEATURE_CLOCKSOURCE2)))
		return;

	kprintf("pvclock_init: will use a%sstable pv_clock as timecounter\n",
	    data.reg[0] & (1 << KVM_FEATURE_CLOCKSOURCE_STABLE_BIT) ? " " :
								      "n un");

	using_pvclock = true;
	(void)vm_page_alloc(&page,
	    vm_bytes_to_order(sizeof(*vcpu_time_info) * ncpus), kPageUseKWired,
	    true);

	vcpu_time_info = (volatile void *)P2V(vm_page_paddr(page));
	ke_tc_set_get_nanos(pvclock_get_nanos);
}

void
pvclock_cpu_init(void)
{
	volatile struct pvclock_vcpu_time_info *info;

	if (!using_pvclock)
		return;

	info = &vcpu_time_info[KCPU_LOCAL_LOAD(cpu)->num];

	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, V2P(info) | 1);
}

static nanosecs_t
pvclock_get_nanos(void)
{
	volatile struct pvclock_vcpu_time_info *info;
	uint32_t version;
	uint64_t ns;

	kassert(using_pvclock);

	asm("cli");

	info = &vcpu_time_info[KCPU_LOCAL_LOAD(cpu)->num];

	do {
		version = __atomic_load_n(&info->version, __ATOMIC_ACQUIRE);
		ns = rdtsc_serialised();
		ns -= info->tsc_timestamp;
		if (info->tsc_shift < 0)
			ns >>= -info->tsc_shift;
		else
			ns <<= info->tsc_shift;
		asm volatile("mulq %%rdx; shrd $32, %%rdx, %%rax"
			     : "=a"(ns)
			     : "a"(ns), "d"(info->tsc_to_system_mul));
		ns += info->system_time;
	} while ((info->version & 1) != 0 || info->version != version);

	asm("sti");

	return ns;
}
