#ifndef KRX_AARCH64_MMU_REGS_H
#define KRX_AARCH64_MMU_REGS_H

#include <stdint.h>

struct __attribute__((packed)) id_aa64mmfr0_el1 {
	uint64_t pa_range : 4, asid_bits : 4, big_end : 4, sns_mem : 4,
	    big_end_el0 : 4, tgran16 : 4, tgran64 : 4, tgran4 : 4,
	    tgran16_2 : 4, tgran64_2 : 4, tgran4_2 : 4, exs : 4, reserved_0 : 8,
	    fgt : 4, ecv : 4;
};

enum tg1_granule {
	kTG1GranuleSize16KiB = 0b01,
	kTG1GranuleSize4KiB = 0b10,
	kTG1GranuleSize64KiB = 0b11,
};

enum tg0_granule {
	kTG0GranuleSize4KiB = 0b00,
	kTG0GranuleSize64KiB = 0b01,
	kTG0GranuleSize16KiB = 0b10,
};

/*! page table entry, hardware */
typedef struct __attribute__((packed)) pte_hw {
	uint64_t valid : 1,
	    /* must = 1 */
	    reserved_must_be_1 : 1,
	    /* attribute index */
	    attrindx : 3,
	    /* nonsecure */
	    ns : 1,
	    /* data access permissions */
	    ap : 2,
	    /* shareability */
	    sh : 2,
	    /* access flag */
	    af : 1,
	    /* nonglobal */
	    ng : 1,
	    /* page frame number */
	    pfn : 36,
	    /* reserved, must = 0 */
	    reserved_0 : 4,
	    /* does this form part of a contiguous set of entries */
	    contiguous : 1,
	    /* privileged execute never */
	    pxn : 1,
	    /* execute never */
	    uxn : 1,
	    /* for us to use */
	    reserved_soft : 4, ignored : 5;
} pte_hw_t;

struct __attribute__((packed)) table_entry {
	uint64_t valid : 1, is_table : 1,
	    ignored : 10, /* 12 for 16kib, 14 for 64 kib */
	    next_level : 36, reserved_0 : 4, ignored_2 : 7, pxntable : 1,
	    xntable : 1, aptable : 2, nstable : 1;
};

struct __attribute__((packed)) tcr_el1 {
	uint64_t t0sz : 6, reserved_0 : 1, epd0 : 1, irgn0 : 2, orgn0 : 2,
	    sh0 : 2;
	enum tg0_granule tg0 : 2;

	uint64_t t1sz : 6, a1 : 1, epd1 : 1, irgn1 : 2, orgn1 : 2, sh1 : 2;
	enum tg1_granule tg1 : 2;

	uint64_t ips : 3, reserved_1 : 1, as : 1, tbi0 : 1, tbi1 : 1, ha : 1,
	    hd : 1, hpd0 : 1, hpd11 : 1, hwu059 : 1, hwu060 : 1, hwu061 : 1,
	    hwu062 : 1, hwu159 : 1, hwu160 : 1, hwu161 : 1, hwu162 : 1,
	    tbid0 : 1, tbid1 : 1, nfd0 : 1, nfd1 : 1, e0pd0 : 1, e0pd1 : 1,
	    tcma0 : 1, tcma1 : 1, ds : 1, reserved_2 : 4;
};

static inline struct id_aa64mmfr0_el1
read_id_aa64mmfr0_el1(void)
{
	struct id_aa64mmfr0_el1 res;
	asm("mrs %0, id_aa64mmfr0_el1" : "=r"(res));
	return res;
}

static inline struct tcr_el1
read_tcr_el1(void)
{
	struct tcr_el1 res;
	asm("mrs %0, tcr_el1" : "=r"(res));
	return res;
}

#endif /* KRX_AARCH64_MMU_REGS_H */
