#ifndef KRX_AMD64_PORTIO_H
#define KRX_AMD64_PORTIO_H

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

#endif /* KRX_AMD64_PORTIO_H */
