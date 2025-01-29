/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Jan 29 2025.
 */

#ifndef KRX_USB_XHCI_REG_H
#define KRX_USB_XHCI_REG_H

#include <ddk/safe_endian.h>

/* 5.3 Host Controller Capability Registers */
struct __attribute__((packed)) xhci_host_cap_regs {
	uint8_t CAPLENGTH;
	uint8_t reserved;
	leu16_t HCIVERSION;
	leu32_t HCSPARAMS1;
	leu32_t HCSPARAMS2;
	leu32_t HCSPARAMS3;
	leu32_t HCCPARAMS1;
	leu32_t DBOFF;
	leu32_t RTSOFF;
	leu32_t HCCPARAMS2;
};

/* 5.4 table 5-19 Host Controller USB Port Register Set */
struct __attribute__((packed)) xhci_port_regs {
	leu32_t PORTSC;
	leu32_t PORTPMSC;
	leu32_t PORTLI;
	leu32_t PORTHLPMC;
};

/* 5.4 Host Controller Operational Registers */
struct __attribute__((packed)) xhci_host_op_regs {
	leu32_t USBCMD;
	leu32_t USBSTS;
	leu32_t PAGESIZE;
	uint8_t reserved1[8];
	leu32_t DNCTRL;
	leu64_t CRCR;
	uint8_t reserved2[16];
	leu64_t DCBAAP;
	leu32_t CONFIG;
	uint8_t reserved3[964];
	struct xhci_port_regs ports[0];
};

/* 5.5.2 Interrupter Register Set */
struct __attribute__((packed)) xhci_interrupt_regs {
	leu32_t IMAN;
	leu32_t IMOD;
	leu32_t ERSTSZ;
	uint8_t reserved[4];
	leu64_t ERSTBA;
	leu32_t ERDP_lo;
	leu32_t ERDP_hi;
};

/* 5.5 Host Controller Runtime Registers */
struct __attribute__((packed)) xhci_host_rt_regs {
	leu32_t MFINDEX;
	uint8_t reserved[28];
	struct xhci_interrupt_regs IR[0];
};

#endif /* KRX_USB_XHCI_REG_H */
