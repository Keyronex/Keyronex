#ifndef KRX_AMD64_GDT_H
#define KRX_AMD64_GDT_H

#include <stdint.h>

typedef struct tss {
	uint32_t reserved;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist1;
	uint64_t ist2;
	uint64_t ist3;
	uint64_t ist4;
	uint64_t ist5;
	uint64_t ist6;
	uint64_t ist7;
	uint64_t reserved2;
	uint32_t iopb;
} __attribute__((packed)) tss_t;

#endif /* KRX_AMD64_GDT_H */
