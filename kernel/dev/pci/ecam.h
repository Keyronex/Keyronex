/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Nov 04 2024.
 */

#ifndef KRX_PCI_ECAM_H
#define KRX_PCI_ECAM_H

#include <kdk/vm.h>

struct ecam_span {
	paddr_t base;
	uint16_t seg;
	uint8_t bus_start, bus_end;
};

vaddr_t ecam_get_view(uint16_t segment, uint8_t bus);

extern struct ecam_span *ecam_spans;
extern size_t ecam_spans_n;

#endif /* KRX_PCI_ECAM_H */
