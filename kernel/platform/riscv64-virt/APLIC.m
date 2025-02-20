/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Thu Aug 08 2024.
 */
/*!
 * @file APIC.m
 * @brief Basic APLIC driver.
 *
 * At present only supports a single APLIC and only in direct mode
 */

#include <kdk/executive.h>
#include <kdk/kmem.h>
#include <kdk/object.h>
#include <kdk/riscv64.h>
#include <kdk/vm.h>
#include <uacpi/acpi.h>

#include "APLIC.h"
#include "dev/acpi/DKACPIPlatform.h"
#include "kern/ki.h"

struct __attribute__((packed)) aplic_mmio {
	uint32_t domaincfg;
	uint32_t sourcecfg[1023];
	const uint8_t reserved[3008];

	uint32_t msiaddrcfg;
	uint32_t msiaddrcfgh;

	uint32_t smsiaddrcfg;
	uint32_t smsiaddrcfgh;
	const char reserved_2[48];

	uint32_t setip[55];
	uint32_t setipnum;
	const uint8_t reserved_3[32];

	uint32_t in_clrip[55];
	uint32_t clripnum;
	const char reserved_4[32];

	uint32_t setie[55];
	uint32_t setienum;
	const uint8_t reserved_5[32];

	uint32_t clrie[55];
	uint32_t clear_irq_enable;
	const uint8_t reserved_6[32];

	uint32_t setipnum_le;
	uint32_t setipnum_be;
	const uint8_t reserved_7[4088];

	uint32_t genmsi;
	uint32_t target[1023];

	struct __attribute__((packed)) idc_mmio {
		uint32_t idelivery;
		uint32_t iforce;
		uint32_t ithreshold;
		uint32_t reserved[3];
		uint32_t topi;
		uint32_t claimi;
	} idc[128];
};

enum {
	kAPLICSourcefgEdge1 = 4,  /* rising edge */
	kAPLICSourcefgEdge0 = 5,  /* falling edge */
	kAPLICSourcefgLevel1 = 6, /* level high */
	kAPLICSourcefgLevel0 = 7, /* level low */
};

static uint32_t
read_u32(volatile void *addr)
{
	return *(volatile uint32_t *)addr;
}

static void
write_u32(volatile void *addr, uint32_t val)
{
	*(volatile uint32_t *)addr = val;
}

static inline uint32_t
target_value(uint32_t hart_id)
{
	return (hart_id << 18) | 1;
}

static inline uint16_t
cpu_idc_index(kcpu_t *cpu)
{
	return cpu->cpucb.aplic_idc_hartindex;
}

static TAILQ_HEAD(intr_entries, intr_entry) intr_entries[1024];
APLIC *aplic;
volatile struct aplic_mmio *mmio;

@implementation APLIC

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry
{
	uint16_t idc_index = cpu_idc_index(&bootstrap_cpu);
	uint32_t gsi = source->id;
	int sourcecfg;

	/* GSI 0 is invalid. Registers describe GSIs starting at 1. */
	gsi--;

	if (source->edge)
		sourcecfg = source->low_polarity ? kAPLICSourcefgLevel0 :
						   kAPLICSourcefgLevel1;
	else
		sourcecfg = source->low_polarity ? kAPLICSourcefgEdge0 :
						   kAPLICSourcefgEdge1;

	write_u32(&aplic->m_mmio->sourcecfg[gsi], sourcecfg);
	write_u32(&aplic->m_mmio->target[gsi], target_value(idc_index));
	write_u32(&aplic->m_mmio->setienum, gsi + 1);

	/* these need to move into per-cpu setup */
	/* enable delivery */
	write_u32(&aplic->m_mmio->idc[idc_index].idelivery, 1);
	/* disable threshold */
	write_u32(&aplic->m_mmio->idc[idc_index].ithreshold, 0);

	entry->name = "gsi";
	entry->ipl = prio;
	entry->handler = handler;
	entry->arg = arg;
	entry->shareable = !source->edge;
	TAILQ_INSERT_TAIL(&intr_entries[gsi + 1], entry, queue_entry);

	return 0;
}

- (instancetype)initWithProvider:(DKACPIPlatform *)provider
		  madtAplicEntry:(struct acpi_madt_aplic *)apic_entry
{
	int r;
	vaddr_t vaddr;

	self = [super init];
	kmem_asprintf(&m_name, "apic-%d", apic_entry->id);

	r = vm_ps_map_physical_view(kernel_process->vm, &vaddr,
	    sizeof(struct aplic_mmio), apic_entry->address, kVMAll, kVMAll,
	    false);
	kassert(r == 0);

	for (int i = 0; i < elementsof(intr_entries); i++) {
		TAILQ_INIT(&intr_entries[i]);
	}

	m_mmio = (void *)vaddr;
	mmio = m_mmio;
	aplic = self;

	write_u32(&m_mmio->domaincfg, 1UL << 8);

	return self;
}

@end

void
aplic_irq(md_intr_frame_t *frame)
{
	ipl_t ipl;
	struct intr_entries *entries;
	struct intr_entry *entry;
	kcpu_t *cpu;
	uint32_t irq;

	cpu = curcpu();
	irq = read_u32(&mmio->idc[cpu->cpucb.aplic_idc_hartindex].claimi) >> 16;

	if (irq == 0)
		return; /* spurious */

	entries = &intr_entries[irq];

	if (TAILQ_EMPTY(entries)) {
		kfatal("Unhandled interrupt %u.", irq);
	}

	ipl = splraise(kIPLHigh);

	TAILQ_FOREACH (entry, entries, queue_entry) {
		bool r = entry->handler(frame, entry->arg);
		(void)r;
	}

	splx(ipl);
	ki_disable_interrupts();
	return;
}
