#ifndef KRX_DEV_DKAACPIPLATFORM_H
#define KRX_DEV_DKAACPIPLATFORM_H

#include <acpispec/tables.h>

#include "ddk/DKDevice.h"

typedef struct {
	char Signature[8];
	uint8_t Checksum;
	char OEMID[6];
	uint8_t Revision;
	uint32_t RsdtAddress;
} __attribute__((packed)) rsdp_desc_t;

typedef struct {
	rsdp_desc_t firstPart;

	uint32_t Length;
	uint64_t XsdtAddress;
	uint8_t ExtendedChecksum;
	uint8_t reserved[3];
} __attribute__((packed)) rsdp_desc2_t;

typedef struct {
	acpi_header_t header;
	uint64_t reserved;
	struct __attribute__((packed)) allocation {
		uint64_t Base;
		uint16_t Segn;
		uint8_t StartBus;
		uint8_t EndBus;
		uint32_t reserved;
	} allocations[0];

} __attribute__((packed)) mcfg_t;

@interface DKACPIPlatform : DKDevice

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp;

@end

#endif /* KRX_DEV_DKAACPIPLATFORM_H */
