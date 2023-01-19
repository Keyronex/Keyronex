#ifndef AMD64_H_
#define AMD64_H_

#include <stdint.h>

enum {
	kAMD64MSRAPICBase = 0x1b,
	kAMD64MSRTSCDeadline = 0x6e0,
	kAMD64MSRGSBase = 0xc0000101,
	kAMD64MSRKernelGSBase = 0xc0000102,
	kAMD64MSRFSBase = 0xc0000100
};

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

#define REG_FUNCS(type, regname)                                    \
	static inline type read_##regname()                         \
	{                                                           \
		type val;                                           \
		asm volatile("mov %%" #regname ", %0" : "=r"(val)); \
		return val;                                         \
	}                                                           \
	static inline void write_##regname(type val)                \
	{                                                           \
		asm volatile("mov %0, %%" #regname ::"a"(val));     \
	}


static inline void
outb(uint16_t port, uint8_t data)
{
	asm volatile("outb %0, %1" ::"a"(data), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t data;
	asm volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
	return data;
}

static inline void
outw(uint16_t port, uint16_t data)
{
	asm volatile("outw %0, %1" ::"a"(data), "Nd"(port));
}

static inline uint16_t
inw(uint16_t port)
{
	uint16_t data;
	asm volatile("inw %1, %0" : "=a"(data) : "Nd"(port));
	return data;
}

static inline void
outl(uint16_t port, uint32_t data)
{
	asm volatile("outl %0, %1" ::"a"(data), "Nd"(port));
}

static inline uint32_t
inl(uint16_t port)
{
	uint32_t data;
	asm volatile("inl %1, %0" : "=a"(data) : "Nd"(port));
	return data;
}

static inline void
wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t high = value >> 32;
	uint32_t low = value;

	asm volatile("wrmsr" ::"c"(msr), "d"(high), "a"(low));
}

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t high, low;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr) : "memory");
	return ((uint64_t)high << 32) | low;
}

REG_FUNCS(uint64_t, cr0);
REG_FUNCS(uint64_t, cr2);
REG_FUNCS(uint64_t, cr3);
REG_FUNCS(uint64_t, cr4);

#endif /* AMD64_H_ */
