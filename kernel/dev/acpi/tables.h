#ifndef KRX_ACPI_TABLES_H
#define KRX_ACPI_TABLES_H

#include "DKAACPIPlatform.h"
#include "uacpi/acpi.h"

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

#endif /* KRX_ACPI_TABLES_H */
