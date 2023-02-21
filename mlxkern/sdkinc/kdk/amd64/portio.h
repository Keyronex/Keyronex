#ifndef MLX_AMD64_PORTIO_H
#define MLX_AMD64_PORTIO_H

#include <stdint.h>

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

#endif /* MLX_AMD64_PORTIO_H */
