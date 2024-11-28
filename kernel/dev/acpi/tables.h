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

typedef struct acpi_table_gtdt {
	acpi_header_t header;
	uint64_t counter_block_address;
	uint32_t reserved;
	uint32_t secure_el1_interrupt;
	uint32_t secure_el1_flags;
	uint32_t nonsecure_el1_interrupt;
	uint32_t nonsecure_el1_flags;
	uint32_t virtual_timer_interrupt;
	uint32_t virtual_timer_flags;
	uint32_t nonsecure_el2_interrupt;
	uint32_t nonsecure_el2_flags;
	uint64_t counter_read_block_address;
	uint32_t platform_timer_count;
	uint32_t platform_timer_offset;
} __attribute__((packed)) acpi_table_gtdt_t;

/* Is it edge triggered? */
#define ACPI_GTDT_INTERRUPT_MODE (1)
/*! Is it active low? */
#define ACPI_GTDT_INTERRUPT_POLARITY (1 << 1)

#endif /* KRX_ACPI_TABLES_H */
