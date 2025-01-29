/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Jan 29 2025.
 */

#include <ddk/DKPCIDevice.h>
#include <kdk/kern.h>
#include <kdk/libkern.h>
#include <kdk/vm.h>

#include "vm/vmp.h"
#include "xhci_reg.h"

@interface XHCIController : DKDevice <DKPCIDeviceMatching> {
	DKPCIDevice *m_pciDevice;

	vaddr_t m_mmioBase;
	volatile struct xhci_host_cap_regs *m_capRegs;
	volatile struct xhci_host_op_regs *m_opRegs;
	volatile struct xhci_host_rt_regs *m_rtRegs;
	volatile leu32_t *m_doorBells;
}

+ (void)load;

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

@end

@implementation XHCIController

+ (void)load
{
	[DKPCIDevice registerMatchingClass:self];
}

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData
{
	if (matchData->class == 0xc && matchData->subclass == 0x3 &&
	    matchData->prog_if == 0x30) {
		return 127;
	}

	return 0;
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
{
	if ((self = [super init])) {
		m_pciDevice = pciDevice;
		m_name = strdup("xhciController");
	}

	return self;
}

- (void)start
{
	DKPCIBarInfo barInfo = [m_pciDevice barInfo:0];
	int r;

	r = vm_ps_map_physical_view(&kernel_procstate, &m_mmioBase,
	    PGROUNDUP(barInfo.size), barInfo.base, kVMRead | kVMWrite,
	    kVMRead | kVMWrite, false);
	kassert(r == 0);

	m_capRegs = (volatile struct xhci_host_cap_regs *)m_mmioBase;
	m_opRegs = (volatile struct xhci_host_op_regs *)(m_mmioBase +
	    m_capRegs->CAPLENGTH);
	m_rtRegs = (volatile struct xhci_host_rt_regs *)(m_mmioBase +
	    from_leu32(m_capRegs->RTSOFF));
	m_doorBells = (volatile leu32_t *)(m_mmioBase +
	    from_leu32(m_capRegs->DBOFF));
}

@end
