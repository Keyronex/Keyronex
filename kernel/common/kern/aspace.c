/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file aspace.c
 * @brief Address space and TLB management.
 *
 * Active set
 * ----------
 *
 * Before an address space is loaded on some CPU, the bit for that CPU is set,
 * and it's cleared only after another is loaded.
 *
 * Consider this sequence:
 *
 * Switching CPU:
 *  1. sets its bit in the active set.
 *  2. loads the new root table.
 *  3. can now read a PTE
 *
 * Invalidating CPU:
 *  1. modifies a PTE.
 *  2. does a seq_cst fence.
 *  3. reads active CPU set to choose shootdown targets.
 *
 * That order makes sure that at least one of switcher and invalidator sees the
 * other's change. Either the invalidator sees the switcher's bit and shoots him
 * down, or switcher sees the updated PTE before it can use the address space,
 * so it didn't need a shootdown.
 */

#include <sys/k_aspace.h>
#include <sys/k_cpumask.h>
#include <sys/k_log.h>
#include <sys/k_xcall.h>
#include <sys/vm.h>

#define KE_INVAL_PAGES_MAX 32 /* if more than this, invalidate everything */
#define KE_INVAL_ALL ((size_t)-1)

struct inval_args {
	const struct ke_aspace *as;
	vaddr_t va;
	size_t npages; /* 0 = everything */
	unsigned flags;
};

void
ke_aspace_init(struct ke_aspace *as, paddr_t root, bool kernel)
{
	as->root = root;
	as->kernel = kernel;
	ke_atomic_cpumask_zero(&as->active_cpus);
	atomic_store_explicit(&as->invalidate_gen, 0, memory_order_relaxed);
}

void
ke_aspace_activate(struct ke_aspace *old, struct ke_aspace *new)
{
	ipl_t ipl;
	kcpunum_t cpu;

	if (old == new)
		return;

	ipl = spldisp();
	cpu = CPU_LOCAL_LOAD(cpu_num);
	ke_atomic_cpumask_set(&new->active_cpus, cpu, memory_order_seq_cst);
	kep_arch_aspace_load(new->root);
	if (old != NULL)
		ke_atomic_cpumask_clear(&old->active_cpus, cpu,
		    memory_order_seq_cst);
	splx(ipl);
}

static void
inval_local(void *arg)
{
	struct inval_args *a = arg;

	kep_arch_tlb_invalidate(a->as, a->va, a->npages, a->flags);
}

void
ke_aspace_invalidate(struct ke_aspace *as, vaddr_t va, size_t len,
    unsigned flags)
{
	extern int kern_initlevel;
	struct inval_args a = { .as = as, .va = 0, .npages = 0, .flags = flags };

	if (len != KE_INVAL_ALL) {
		vaddr_t start = va & ~((vaddr_t)PGSIZE - 1);
		vaddr_t end = roundup2(va + len, PGSIZE);

		kassert(as->kernel || end <= HIGHER_HALF,
		    "cannot invalidate higher-half address in user aspace");
		a.va = start;
		a.npages = (end - start) >> PGSHIFT;
		if (a.npages > KE_INVAL_PAGES_MAX)
			a.npages = 0;
	}

	if (kern_initlevel >= 2 && /* i.e. SMP */
	    !kep_arch_tlb_invalidate_remote(as, a.va, a.npages, a.flags))
		ke_xcall_broadcast(inval_local, &a);
	inval_local(&a);

	atomic_fetch_add_explicit(&as->invalidate_gen, 1, memory_order_release);
}

void
ke_aspace_invalidate_all(struct ke_aspace *as, unsigned flags)
{
	ke_aspace_invalidate(as, 0, KE_INVAL_ALL, flags);
}
