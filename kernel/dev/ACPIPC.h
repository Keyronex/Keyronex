#ifndef ACPIPC_H_
#define ACPIPC_H_

#include <acpispec/tables.h>
#include <devicekit/DKDevice.h>

typedef struct {
	char	 Signature[8];
	uint8_t	 Checksum;
	char	 OEMID[6];
	uint8_t	 Revision;
	uint32_t RsdtAddress;
} __attribute__((packed)) rsdp_desc_t;

typedef struct {
	rsdp_desc_t firstPart;

	uint32_t Length;
	uint64_t XsdtAddress;
	uint8_t	 ExtendedChecksum;
	uint8_t	 reserved[3];
} __attribute__((packed)) rsdp_desc2_t;

typedef struct {
	acpi_header_t header;
	uint64_t reserved;
	struct allocation {
		uint64_t Base;
		uint64_t Segn;
		uint8_t StartBus;
		uint8_t EndBus;
		uint32_t reserved;
	} allocations[0];

} __attribute__((packed)) mcfg_t;

@interface AcpiPC : DKDevice

+ (AcpiPC*)instance;

+ (BOOL)probeWithRSDP:(rsdp_desc_t *)rsdp;

- init;

@end

#endif /* ACPIPC_H_ */
