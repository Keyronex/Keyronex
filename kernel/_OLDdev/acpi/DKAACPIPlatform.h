#ifndef KRX_DEV_DKAACPIPLATFORM_H
#define KRX_DEV_DKAACPIPLATFORM_H

#include "ddk/DKDevice.h"

struct acpi_madt;
struct acpi_entry_hdr;

typedef struct {
	char Signature[8];
	uint8_t Checksum;
	char OEMID[6];
	uint8_t Revision;
	uint32_t RsdtAddress;
} __attribute__((packed)) rsdp_desc_t;

@interface DKACPIPlatform : DKDevice

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp;
+ (instancetype)instance;

- (void)secondStageInit;

/* These are for subclasses to override. */
- (void)iterateArchSpecificEarlyTables;

@end

void dk_acpi_madt_walk(struct acpi_madt *madt,
    void (*callback)(struct acpi_entry_hdr *item, void *arg), void *arg);

#endif /* KRX_DEV_DKAACPIPLATFORM_H */
