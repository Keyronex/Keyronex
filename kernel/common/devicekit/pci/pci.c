/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024-26 Cloudarox Solutions.
 */
/*!
 * @file pci.c
 * @brief PCI access
 */

#include <devicekit/pci/DKPCIDevice.h>

#if defined(__amd64__)
#include <asm/io.h>

uint8_t
pci_readb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inb(0xCFC + (offset & 3));
}

uint16_t
pci_readw(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inw(0xCFC + (offset & 3));
}

uint32_t
pci_readl(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inl(0xCFC + (offset & 3));
}

void
pci_writeb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint8_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outb(0xCFC + (offset & 3), value);
}

void
pci_writew(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint16_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outw(0xCFC + (offset & 3), value);
}

void
pci_writel(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint32_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outl(0xCFC + (offset & 3), value);
}

#else

#define PCI_CONFIG_OFFSET(slot, function, offset) \
	((slot << 15) | (function << 12) | (offset))

uint8_t
pci_readb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint8_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

uint16_t
pci_readw(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint16_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

uint32_t
pci_readl(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	return *(volatile uint32_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset));
}

void
pci_writeb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint8_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint8_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

void
pci_writew(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint16_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint16_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

void
pci_writel(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint32_t value)
{
	vaddr_t ecam = ecam_get_view(seg, bus);
	*(volatile uint32_t *)(ecam +
	    PCI_CONFIG_OFFSET(slot, function, offset)) = value;
}

#endif
