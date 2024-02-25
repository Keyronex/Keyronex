#include <stdint.h>

#include "goldfish.h"
#include "kdk/nanokern.h"

enum gfpic_reg {
	kGFPICRegStatus = 0x00,
	kGFPICRegPending = 0x04,
	kGFPICRegDisableAll = 0x08,
	kGFPICRegDisable = 0x0c,
	kGFPICRegEnable = 0x10,
};

struct gfpic {
	bool (*handler[32])(md_intr_frame_t *, void *);
	void *arg[32];
};

#define GFPIC_COUNT 6
#define GFPIC_IRQBASE 1
#define GFPIC_PIC(IRQ) (IRQ / 32)
#define GFPIC_PICIRQ(IRQ) (IRQ % 32)

/*! hardcoded value i found in qemu, lmao */
volatile char *gfpic_regs = (void *)0xff000000;
static struct gfpic pics[GFPIC_COUNT];

static uint32_t
gfpic_read(unsigned int pic, unsigned int reg)
{
	return *((uint32_t *)&gfpic_regs[pic * 0x1000 + reg]);
}

static void
gfpic_write(unsigned int pic, enum gfpic_reg reg, uint32_t val)
{
	*((uint32_t *)&gfpic_regs[pic * 0x1000 + reg]) = val;
}

static void
dump_pic(unsigned int i)
{
	kprintf("PIC %d: status %u pending %x\n", i,
	    gfpic_read(i, kGFPICRegStatus), gfpic_read(i, kGFPICRegPending));
}

void
dump_all_pics(void)
{
	for (int i = 0; i < GFPIC_COUNT; i++) {
		dump_pic(i);
	}
}

void
gfpic_mask_irq(unsigned int vector)
{
	gfpic_write(GFPIC_PIC(vector), kGFPICRegDisable,
	    1 << GFPIC_PICIRQ(vector));
}

void
gfpic_unmask_irq(unsigned int vector)
{
	gfpic_write(GFPIC_PIC(vector), kGFPICRegEnable,
	    1 << GFPIC_PICIRQ(vector));
}

void
gfpic_handle_irq(unsigned int vector,
    bool (*handler)(md_intr_frame_t *, void *), void *arg)
{
	pics[GFPIC_PIC(vector)].handler[GFPIC_PICIRQ(vector)] = handler;
	pics[GFPIC_PIC(vector)].arg[GFPIC_PICIRQ(vector)] = arg;
}

void
gfpic_dispatch(unsigned int pic_num, md_intr_frame_t *frame)
{
	while (true) {
		uint32_t pending = gfpic_read(pic_num, kGFPICRegPending), irq;
		if (pending == 0)
			return;
		irq = __builtin_ctz(pending);
		kassert(pics[pic_num].handler[irq] != NULL);
		pics[pic_num].handler[irq](frame,
		    pics[pic_num].arg[irq]);
	}
}
