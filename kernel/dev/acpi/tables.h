#ifndef KRX_ACPI_TABLES_H
#define KRX_ACPI_TABLES_H

#include "DKAACPIPlatform.h"

typedef struct {
	acpi_header_t header;
	uint32_t lapic_addr;
	uint32_t flags;
	uint8_t entries[0];
} __attribute__((packed)) acpi_madt_t;

typedef struct {
	uint8_t type;
	uint8_t length;
} __attribute__((packed)) acpi_madt_entry_header_t;

/* MADT entry type 1 */
typedef struct {
	acpi_madt_entry_header_t header;
	uint8_t ioapic_id;
	uint8_t reserved;
	uint32_t ioapic_addr;
	uint32_t gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* MADT entry type 2 */
typedef struct {
	acpi_madt_entry_header_t header;
	uint8_t bus_source;
	uint8_t irq_source;
	uint32_t gsi;
	uint16_t flags;
} __attribute__((packed)) acpi_madt_int_override_t;

/* MADT entry type 0xb */
typedef struct {
	acpi_madt_entry_header_t header;
	uint16_t reserved;
	uint32_t cpu_interface_number;
	uint32_t acpi_process_uid;
	uint32_t flags;
	uint32_t parking_protocol_version;
	uint32_t performance_interrupt_gsiv;
	uint64_t parked_address;
	uint64_t physical_base_addr;
	uint64_t gicv;
	uint64_t gich;
	uint32_t vgic_maintenance_intr;
	uint64_t gicr_base_addr;
	uint64_t mpidr;
	uint8_t processor_power_efficiency_class;
	uint8_t reserved_0; /* must be zero */
	uint16_t spe_overflow_interrupt;
	uint16_t trbe_interrupt;
} __attribute__((packed)) acpi_madt_gicc_t;

typedef struct {
	acpi_madt_entry_header_t header;
	uint16_t reserved;
	uint32_t gic_id;
	uint64_t physical_base_address;
	uint32_t system_vector_base;
	uint8_t gic_version;
	uint32_t reserved_0 : 24;
} __attribute__((packed)) acpi_madt_gicd_t;

typedef struct acpi_gas_t {
	uint8_t address_space;
	uint8_t bit_width;
	uint8_t bit_offset;
	uint8_t access_size;
	uint64_t base;
} __attribute__((packed)) acpi_gas_t;

typedef struct acpi_fadt_t {
	acpi_header_t header;
	uint32_t firmware_control;
	uint32_t dsdt; // pointer to dsdt

	uint8_t reserved;

	uint8_t profile;
	uint16_t sci_irq;
	uint32_t smi_command_port;
	uint8_t acpi_enable;
	uint8_t acpi_disable;
	uint8_t s4bios_req;
	uint8_t pstate_control;
	uint32_t pm1a_event_block;
	uint32_t pm1b_event_block;
	uint32_t pm1a_control_block;
	uint32_t pm1b_control_block;
	uint32_t pm2_control_block;
	uint32_t pm_timer_block;
	uint32_t gpe0_block;
	uint32_t gpe1_block;
	uint8_t pm1_event_length;
	uint8_t pm1_control_length;
	uint8_t pm2_control_length;
	uint8_t pm_timer_length;
	uint8_t gpe0_length;
	uint8_t gpe1_length;
	uint8_t gpe1_base;
	uint8_t cstate_control;
	uint16_t worst_c2_latency;
	uint16_t worst_c3_latency;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t duty_offset;
	uint8_t duty_width;

	// cmos registers
	uint8_t day_alarm;
	uint8_t month_alarm;
	uint8_t century;

	// ACPI 2.0 fields
	uint16_t iapc_boot_flags;
	uint8_t reserved2;
	uint32_t flags;

	acpi_gas_t reset_register;
	uint8_t reset_command;
	uint16_t arm_boot_flags;
	uint8_t minor_version;

	uint64_t x_firmware_control;
	uint64_t x_dsdt;

	acpi_gas_t x_pm1a_event_block;
	acpi_gas_t x_pm1b_event_block;
	acpi_gas_t x_pm1a_control_block;
	acpi_gas_t x_pm1b_control_block;
	acpi_gas_t x_pm2_control_block;
	acpi_gas_t x_pm_timer_block;
	acpi_gas_t x_gpe0_block;
	acpi_gas_t x_gpe1_block;
	acpi_gas_t sleep_control_reg;
	acpi_gas_t sleep_status_reg;
} __attribute__((packed)) acpi_fadt_t;

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
