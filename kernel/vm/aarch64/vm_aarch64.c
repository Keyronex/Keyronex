#include "kdk/vm.h"
#include "mmu_regs.h"
#include "vm/vmp.h"

union __attribute__((packed)) aarch64_addr {
	struct __attribute__((packed)) {
		uint64_t pgi : 12, l3 : 9, l2 : 9, l1 : 9, l0 : 9, n : 15,
		    ttbr : 1;
	};
	uint64_t addr;
};

struct __attribute__((packed)) table_entry {
	uint64_t valid : 1, is_table : 1,
	    ignored : 10, /* 12 for 16kib, 14 for 64 kib */
	    next_level : 36, reserved_0 : 4, ignored_2 : 7, pxntable : 1,
	    xntable : 1, aptable : 2, nstable : 1;
};

static inline void *
read_ttbr0_el1(void)
{
	void *res;
	asm("mrs %0, ttbr0_el1" : "=r"(res));
	return res;
}

static inline void *
read_ttbr1_el1(void)
{
	void *res;
	asm("mrs %0, ttbr1_el1" : "=r"(res));
	return res;
}

void vmem_earlyinit(void);
int internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out);
void internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags);

vm_procstate_t kernel_procstate;
vmem_t vmem_kern_nonpaged;

void
vmp_kernel_init(void)
{
	struct id_aa64mmfr0_el1 id = read_id_aa64mmfr0_el1();
	struct tcr_el1 tcr = read_tcr_el1();

	if (id.tgran4 != 0) {
		kfatal("todo: support non 4kib-granularity pages\n")
	} else if (tcr.tg0 != kTG0GranuleSize4KiB ||
	    tcr.tg1 != kTG1GranuleSize4KiB) {
		kfatal("todo: support non 4kib-granularity pages\n")
	} else if (tcr.t0sz != 16 || tcr.t1sz != 16) {
		kfatal("todo: support non-16 t0sz/t1sz\n");
	}

	vmem_earlyinit();

	vmem_init(&kernel_procstate.vmem, "kernel-va", KVM_DYNAMIC_BASE,
	    KVM_DYNAMIC_SIZE, PGSIZE, NULL, NULL, NULL, 0, kVMemBootstrap,
	    kIPL0);
	vmem_init(&vmem_kern_nonpaged, "kernel-nonpaged", 0, 0, PGSIZE,
	    internal_allocwired, internal_freewired, &kernel_procstate.vmem, 0,
	    kVMemBootstrap, kIPL0);

	ke_mutex_init(&kernel_procstate.mutex);
	RB_INIT(&kernel_procstate.vad_queue);
}

int
vmp_md_enter_kwired(vaddr_t virt, paddr_t phys)
{
	struct table_entry *l0, *l1, *l2;
	pte_t *l3;
	union aarch64_addr addr;

#if 0
(gdb) print l3[addr.l3]
$1 = {valid = 1, reserved_must_be_1 = 1, attrindx = 0, ns = 0, ap = 2, sh = 3, af = 1, ng = 0, page = 264656,
  reserved_0 = 0, contiguous = 0, pxn = 0, uxn = 0, reserved_soft = 0, ignored = 0}
#endif

	addr.addr = virt;
	l0 = (void*)P2V(addr.ttbr == 1 ? read_ttbr1_el1() : read_ttbr0_el1());

	if (l0[addr.l0].valid == 0) {
		vm_page_t *l1_page;
		int r = vmp_page_alloc_locked(&l1_page, kPageUsePML3, false);
		uintptr_t l1_phys;

		if (r != 0)
			return r;

		l1_page->refcnt = 0;
		l1_page->nonzero_ptes = 0;

		l0[addr.l0].valid = 1;
		l0[addr.l0].is_table = 1;
		l0[addr.l0].next_level = vm_page_paddr(l1_page) >> 12;

		l1 = (void*)P2V(vm_page_paddr(l1_page));
	} else {
		l1 = (void*)P2V(l0[addr.l0].next_level << 12);
	}

	if (l1[addr.l1].valid == 0) {
		vm_page_t *l2_page;
		int r = vmp_page_alloc_locked(&l2_page, kPageUsePML2, false);
		uintptr_t l2_phys;

		if (r != 0)
			return r;

		l2_page->refcnt = 0;
		l2_page->nonzero_ptes = 0;

		l1[addr.l1].valid = 1;
		l1[addr.l1].is_table = 1;
		l1[addr.l1].next_level = vm_page_paddr(l2_page) >> 12;

		l2 = (void*)P2V(vm_page_paddr(l2_page));
	} else {
		l2 = (void*)P2V(l1[addr.l1].next_level << 12);
	}

	if (l2[addr.l2].valid == 0) {
		vm_page_t *l3_page;
		int r = vmp_page_alloc_locked(&l3_page, kPageUsePML1, false);
		uintptr_t l3_phys;

		if (r != 0)
			return r;

		l3_page->refcnt = 0;
		l3_page->nonzero_ptes = 0;

		l2[addr.l2].valid = 1;
		l2[addr.l2].is_table = 1;
		l2[addr.l2].next_level = vm_page_paddr(l3_page) >> 12;

		l3 = (void*)P2V(vm_page_paddr(l3_page));
	} else {
		l3 = (void*)P2V(l2[addr.l2].next_level << 12);
	}

	//kprintf("Mapping virt 0x%zx to phys 0x%zx: %d/%d/%d/%d\n", virt, phys, addr.l0, addr.l1, addr.l2, addr.l3);
	kassert(l3[addr.l3].hw.valid == 0);
	l3[addr.l3].hw.page = phys >> 12;
	l3[addr.l3].hw.valid = 1;
	l3[addr.l3].hw.reserved_must_be_1 = 1;
	l3[addr.l3].hw.sh = 3;
	l3[addr.l3].hw.af = 1;

	return 0;
}

int
vmp_md_trans(vaddr_t virt)
{
	struct table_entry *l0, *l1, *l2;
	 pte_t *l3;
	union aarch64_addr addr;

	addr.addr = virt;
	l0 = (void*)P2V(addr.ttbr == 1 ? read_ttbr1_el1() : read_ttbr0_el1());

	if (l0[addr.l0].valid == 0) {
		kfatal("no l0\n");
	} else {
		l1 = (void*)P2V(l0[addr.l0].next_level << 12);
	}

	if (l1[addr.l1].valid == 0) {
		kfatal("no l1\n");
	} else {
		l2 = (void*)P2V(l1[addr.l1].next_level << 12);
	}

	if (l2[addr.l2].valid == 0) {
		kfatal("No l2\n");
	} else {
		l3 = (void*)P2V(l2[addr.l2].next_level << 12);
	}

	kprintf("0x%zx: %d/%d/%d/%d\n", virt, addr.l0, addr.l1, addr.l2, addr.l3);
	kprintf("At virt 0x%zx: Valid %d\n"
	"\tPhys 0x%zx\n", virt, l3[addr.l3].hw.valid , l3[addr.l3].hw.page << 12);

	return 0;
}


paddr_t
vmp_md_translate(vaddr_t addr)
{
	kfatal("Implement\n");
}


#if 0
int
vmp_md_wire_pte(struct vmp_pte_wire_state *state)
{
	kfatal("Implement please\n");
}
#endif
