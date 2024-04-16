#ifndef KRX_DEV_DKAACPIPLATFORM_H
#define KRX_DEV_DKAACPIPLATFORM_H

#include "ddk/DKDevice.h"

typedef struct acpi_header_t {
	char signature[4];
	uint32_t length;
	uint8_t revision;
	uint8_t checksum;
	char oem[6];
	char oem_table[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed)) acpi_header_t;

typedef struct acpi_rsdt_t {
	acpi_header_t header;
	uint32_t tables[];
} __attribute__((packed)) acpi_rsdt_t;

typedef struct acpi_xsdt_t {
    acpi_header_t header;
    uint64_t tables[];
} __attribute__((packed)) acpi_xsdt_t;

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
