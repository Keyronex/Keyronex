static inline void
write_vbar_el1(void *addr)
{
	asm volatile("msr VBAR_EL1, %0" :: "r"(addr));
}

void c_exception(void* frame)
{
	for (;;) ;
}

void intr_setup(void) {
	extern void *vectors;
	write_vbar_el1(&vectors);
}
