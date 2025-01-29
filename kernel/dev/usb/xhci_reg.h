/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Jan 29 2025.
 */

#ifndef KRX_USB_XHCI_REG_H
#define KRX_USB_XHCI_REG_H

#include <ddk/safe_endian.h>

#define XHCI_HCS2_MAX_SCRATCHPAD(p) ((((p) >> 21) & 0x1f) << 5) | \
    (((p) >> 27) & 0x1f)


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

/* Port Status & Control */
#define XHCI_PORTSC_CCS (1 << 0)  /* Current Connect Status */
#define XHCI_PORTSC_PED (1 << 1)  /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA (1 << 3)  /* Over-current Active */
#define XHCI_PORTSC_PR (1 << 4)	  /* Port Reset */
#define XHCI_PORTSC_PP (1 << 9)	  /* Port Power */
#define XHCI_PORTSC_CSC (1 << 17) /* Connect Status Change */
#define XHCI_PORTSC_PEC (1 << 18) /* Port Enabled/Disabled Change */
#define XHCI_PORTSC_PRC (1 << 21) /* Port Reset Change */

/* 5.4 table 5-19 Host Controller USB Port Register Set */
struct __attribute__((packed)) xhci_port_regs {
	leu32_t PORTSC;
	leu32_t PORTPMSC;
	leu32_t PORTLI;
	leu32_t PORTHLPMC;
};

/* USB Command */
#define XHCI_CMD_RUN (1 << 0)
#define XHCI_CMD_HCRST (1 << 1)
#define XHCI_CMD_INTE (1 << 2)
#define XHCI_CMD_HSEE (1 << 3)

/* USB Status */
#define XHCI_STS_HCH (1 << 0)
#define XHCI_STS_HSE (1 << 2)
#define XHCI_STS_EINT (1 << 3)
#define XHCI_STS_PCD (1 << 4)
#define XHCI_STS_CNR (1 << 11)

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

#define XHCI_IMAN_IP (1 << 0)
#define XHCI_IMAN_IE (1 << 1)

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

/* TRB Types */
#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP_STAGE 2
#define TRB_TYPE_DATA_STAGE 3
#define TRB_TYPE_STATUS_STAGE 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_EVENT_DATA 7
#define TRB_TYPE_CMD_ENABLE_SLOT 9
#define TRB_TYPE_CMD_DISABLE_SLOT 10
#define TRB_TYPE_CMD_ADDRESS_DEV 11
#define TRB_TYPE_CMD_CONFIG_EP 12
#define TRB_TYPE_CMD_RESET_EP 13
#define TRB_TYPE_CMD_STOP_EP 14
#define TRB_TYPE_CMD_SET_TR_DEQ 15
#define TRB_TYPE_CMD_RESET_DEV 16
#define TRB_TYPE_CMD_NOOP 23
#define TRB_TYPE_EVENT_TRANSFER 32
#define TRB_TYPE_EVENT_CMD_COMPLETE 33
#define TRB_TYPE_EVENT_PORT_STATUS 34

/* TRB Flags */
#define TRB_FLAGS_ENT (1 << 1)
#define TRB_FLAGS_ISP (1 << 2)
#define TRB_FLAGS_NS (1 << 3)
#define TRB_FLAGS_CH (1 << 4)
#define TRB_FLAGS_IOC (1 << 5)
#define TRB_FLAGS_IDT (1 << 6)
#define TRB_FLAGS_BEI (1 << 9)

/* TRB Completion Codes */
#define TRB_CC_SUCCESS 1
#define TRB_CC_DATA_BUFFER_ERROR 2
#define TRB_CC_BABBLE_ERROR 3
#define TRB_CC_USB_TRANSACTION_ERROR 4
#define TRB_CC_TRB_ERROR 5
#define TRB_CC_STALL_ERROR 6
#define TRB_CC_RESOURCE_ERROR 7
#define TRB_CC_BANDWIDTH_ERROR 8
#define TRB_CC_NO_SLOTS_ERROR 9
#define TRB_CC_INVALID_STREAM_ERROR 10
#define TRB_CC_SLOT_NOT_ENABLED_ERROR 11
#define TRB_CC_EP_NOT_ENABLED_ERROR 12
#define TRB_CC_SHORT_PACKET 13
#define TRB_CC_RING_UNDERRUN 14
#define TRB_CC_RING_OVERRUN 15
#define TRB_CC_VF_EVENT_RING_FULL 16
#define TRB_CC_PARAMETER_ERROR 17
#define TRB_CC_BANDWIDTH_OVERRUN 18
#define TRB_CC_CONTEXT_STATE_ERROR 19
#define TRB_CC_NO_PING_RESPONSE 20
#define TRB_CC_EVENT_RING_FULL 21
#define TRB_CC_INCOMPATIBLE_DEVICE 22
#define TRB_CC_MISSED_SERVICE 23
#define TRB_CC_CMD_RING_STOPPED 24
#define TRB_CC_CMD_ABORTED 25
#define TRB_CC_STOPPED 26
#define TRB_CC_STOPPED_LENGTH_INVALID 27
#define TRB_CC_MAX_EXIT_LATENCY_ERROR 29
#define TRB_CC_ISOCH_BUFFER_OVERRUN 31
#define TRB_CC_EVENT_LOST 32
#define TRB_CC_UNDEFINED_ERROR 33
#define TRB_CC_INVALID_STREAM_ID 34
#define TRB_CC_SECONDARY_BANDWIDTH 35
#define TRB_CC_SPLIT_TRANSACTION 36

/* Transfer Request Block */
struct __attribute__((packed)) xhci_trb {
	leu64_t params;
	leu32_t status;
	leu32_t control;
};

/* Event Ring Segment Table Entry */
struct __attribute__((packed)) xhci_erst_entry {
	leu64_t ring_segment_base;
	leu32_t ring_segment_size;
	leu32_t reserved;
};

/* Device Context Base Address Array Entry */
struct __attribute__((packed)) xhci_dcbaa_entry {
	leu64_t dev_context_ptr;
};

/* Slot Context */
struct __attribute__((packed)) xhci_slot_context {
	leu32_t field1;
	leu32_t field2;
	leu32_t field3;
	leu32_t field4;
	uint32_t reserved[4];
};

/* Endpoint Context */
struct __attribute__((packed)) xhci_ep_context {
	leu32_t field1;
	leu32_t field2;
	leu64_t tr_dequeue_pointer;
	leu32_t field4;
	uint32_t reserved[3];
};

/* Device Context */
struct __attribute__((packed)) xhci_dev_context {
	struct xhci_slot_context slot;
	struct xhci_ep_context endpoints[31];
};

#endif /* KRX_USB_XHCI_REG_H */
