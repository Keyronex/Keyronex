#include "goldfish.h"
#include "kern/ki.h"

#define VIRT_GF_RTC_IRQ_BASE PIC_IRQ(6, 1)

enum gfrtc_reg {
	GFRTC_TIME_LOW = 0x00,
	GFRTC_TIME_HIGH = 0x04,
	GFRTC_ALARM_LOW = 0x08,
	GFRTC_ALARM_HIGH = 0x0c,
	GFRTC_IRQ_ENABLED = 0x10,
	GFRTC_CLEAR_ALARM = 0x14,
	GFRTC_ALARM_STATUS = 0x18,
	GFRTC_CLEAR_INTERRUPT = 0x1c,
};

/*! hardcoded value i found in qemu, lmao */
const volatile char *gfrtc_regs = (void *)0xff006000;

static uint32_t
gfrtc_read(unsigned int reg)
{
	return *((uint32_t *)&gfrtc_regs[reg]);
}

static void
gfrtc_write(enum gfrtc_reg reg, uint32_t val)
{
	*((uint32_t *)&gfrtc_regs[reg]) = val;
}

uint64_t
gfrtc_get_time(void)
{
	uint32_t lo;
	uint64_t hi;
	lo = gfrtc_read(GFRTC_TIME_LOW);
	hi = gfrtc_read(GFRTC_TIME_HIGH);
	return hi << 32 | lo;
}

void
gfrtc_oneshot(uint64_t ns)
{
	uint64_t deadline = gfrtc_get_time() + ns;
	gfrtc_write(GFRTC_ALARM_HIGH, deadline >> 32);
	gfrtc_write(GFRTC_ALARM_LOW, deadline);
}

bool
gfrtc_handler(md_intr_frame_t *frame, void *arg)
{
	gfrtc_write(GFRTC_CLEAR_ALARM, 1);
	gfrtc_write(GFRTC_CLEAR_INTERRUPT, 1);
	gfrtc_oneshot(NS_PER_S / KERN_HZ);
	ki_cpu_hardclock(frame, NULL);
	return true;
}

void
gfrtc_init(void)
{
	gfrtc_write(GFRTC_IRQ_ENABLED, 1);
	gfpic_handle_irq(VIRT_GF_RTC_IRQ_BASE, gfrtc_handler, NULL);
	gfpic_unmask_irq(VIRT_GF_RTC_IRQ_BASE);
}
