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

/* Port Status & Control(Table 5-27) */
#define XHCI_PORTSC_CCS (1 << 0)  /* Current Connect Status */
#define XHCI_PORTSC_PED (1 << 1)  /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA (1 << 3)  /* Over-current Active */
#define XHCI_PORTSC_PR (1 << 4)	  /* Port Reset */
#define XHCI_PORTSC_PP (1 << 9)	  /* Port Power */
#define XHCI_PORTSC_SPEED(x) ((x >> 10) & 0xf)
/* 7.2.2.1.1 Default USB Speed ID Mapping */
#define XHCI_PORTSC_PS_FS 1
#define XHCI_PORTSC_PS_LS 2
#define XHCI_PORTSC_PS_HS 3
#define XHCI_PORTSC_PS_SS 4
#define XHCI_PORTSC_CSC (1 << 17) /* Connect Status Change */
#define XHCI_PORTSC_PEC (1 << 18) /* Port Enabled/Disabled Change */
#define XHCI_PORTSC_WRC (1 << 19) /* Warm Port Reset Change */
#define XHCI_PORTSC_OCC (1 << 20) /* Over-current Change */
#define XHCI_PORTSC_PRC (1 << 21) /* Port Reset Change */
#define XHCI_PORTSC_PLC (1 << 22) /* Port Link State Change */
#define XHCI_PORTSC_CEC (1 << 23) /* Config Error Change */
#define	XHCI_PORTSC_CMD_BITS_CLEAR 0x80ff01ffU

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
#define TRB_TYPE_CMD_EVALUATE_CONTEXT 13
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

/* Context sizes - 6.1 */
#define CTX_64BYTE 0x0
#define CTX_32BYTE 0x1

/* Slot State - 4.5.3 */
#define SLOT_STATE_DISABLED 0
#define SLOT_STATE_ENABLED 1
#define SLOT_STATE_DEFAULT 2
#define SLOT_STATE_ADDRESSED 3
#define SLOT_STATE_CONFIGURED 4

/* Root Hub Port Speed IDs - 4.19.7 */
#define XHCI_SPEED_FULL 1
#define XHCI_SPEED_LOW 2
#define XHCI_SPEED_HIGH 3
#define XHCI_SPEED_SUPER 4
#define XHCI_SPEED_SUPER_PLUS 5

/* Slot Context Field Accessors */
#define SLOT_CTX_00_ROUTE_STRING(f1) ((f1)&0xFFFFF)
#define SLOT_CTX_00_MTT(f2) (((f2) >> 25) & 0x1)
#define SLOT_CTX_00_HUB(f2) (((f2) >> 26) & 0x1)
#define SLOT_CTX_00_CONTEXT_ENTRIES(f2) (((f2) >> 27) & 0x1F)

#define SLOT_CTX_00_SET_ROUTE_STRING(val) (((val)&0xFFFFF))
#define SLOT_CTX_00_SET_MTT(val) ((((val)&0x1) << 25))
#define SLOT_CTX_00_SET_HUB(val) ((((val)&0x1) << 26))
#define SLOT_CTX_00_SET_CONTEXT_ENTRIES(val) ((((val)&0x1F) << 27))

#define SLOT_CTX_04_SET_ROOT_HUB_PORT(val) (((val)&0xFF) << 16)
#define SLOT_CTX_04_SET_NUM_PORTS(val) (((val)&0xFF) << 24)

#define SLOT_CTX_08_SET_INTERRUPTER_TARGET(val) (((val)&0xF) << 0)

/* Slot Context - 6.2.2 */
struct __attribute__((packed)) xhci_slot_ctx {
	leu32_t field1;
	leu32_t field2;
	leu32_t field3;
	leu32_t field4;
	leu32_t reserved[4];
};

/* Endpoint Context Field Accessors */
#define EP_CTX_INTERVAL(f1) (((f1) >> 16) & 0xFF)
#define EP_CTX_LSA(f1) (((f1) >> 15) & 0x1)
#define EP_CTX_MAX_PSTREAMS(f1) (((f1) >> 10) & 0x1F)
#define EP_CTX_MULT(f1) (((f1) >> 8) & 0x3)
#define EP_CTX_STATE(f1) (((f1) >> 0) & 0x7)

#define EP_CTX_SET_INTERVAL(f1, val) ((f1) |= (((val)&0xFF) << 16))
#define EP_CTX_SET_LSA(f1, val) ((f1) |= (((val)&0x1) << 15))
#define EP_CTX_SET_MAX_PSTREAMS(f1, val) ((f1) |= (((val)&0x1F) << 10))
#define EP_CTX_SET_MULT(f1, val) ((f1) |= (((val)&0x3) << 8))
#define EP_CTX_SET_STATE(f1, val) ((f1) |= (((val)&0x7) << 0))

/* Endpoint Context - 6.2.3 */
struct __attribute__((packed)) xhci_ep_ctx {
	leu32_t field1;
	leu32_t field2;
	leu64_t dequeue_ptr;
	leu32_t tx_info;
	leu32_t reserved[3];
};

/* Input Control Context - 6.2.5.1 */
struct __attribute__((packed)) xhci_input_control_ctx {
	leu32_t drop_flags;
	leu32_t add_flags;
	leu32_t reserved[6];
};

/* Input Context - 6.2.5 */
struct __attribute__((packed)) xhci_input_ctx {
	struct xhci_input_control_ctx ctrl;
	struct xhci_slot_ctx slot;
	struct xhci_ep_ctx ep[31];
};

/* Device Context */
struct __attribute__((packed)) xhci_device_ctx {
	struct xhci_slot_ctx slot;
	struct xhci_ep_ctx ep[31];
};

/* Max Packet Size based on speed */
#define XHCI_DEFAULT_MAX_PACKET_SIZE_FS 8
#define XHCI_DEFAULT_MAX_PACKET_SIZE_HS 64
#define XHCI_DEFAULT_MAX_PACKET_SIZE_SS 512

#endif /* KRX_USB_XHCI_REG_H */
