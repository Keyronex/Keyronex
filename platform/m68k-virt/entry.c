#include "bootinfo.h"
#include "goldfish.h"
#include "handover.h"
#include "kdk/endian.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "nanokern/ki.h"
#include "executive/exp.h"
#include "vm/vmp.h"

static size_t memory_size = 0;
kcpu_t bootstrap_cpu;
kspinlock_t pac_console_lock = KSPINLOCK_INITIALISER;
struct bootinfo bootinfo;
struct kthread thread0;

struct bootinfo_item {
	uint16_t tag;
	uint16_t size;
	uint32_t data[0];
};

static void
parse_bootinfo(uint8_t *ptr)
{
	for (;;) {
		struct bootinfo_item *item = (struct bootinfo_item *)ptr;

		if (item->tag == 0)
			break;

		switch (item->tag) {
		case 0x1: /* BI_MACHTYPE */
		case 0x2: /* BI_CPUTYPE */
		case 0x3: /* BI_FPUTYPE */
		case 0x4: /* BI_MMUTYPE */
			/* we already know these (?) */
			break;

		case 0x5: /* BI_MEMCHUNK */
			/* 0 base was checked by loader */
			memory_size = item->data[1];
			break;

		case 0x8000: /* BI_VIRT_QEMU_VERSION */
			bootinfo.qemu_version = item->data[0];
			break;

		case 0x8001:   /* BI_VIRT_GF_PIC_BASE */
		case 0x8002:   /* BI_VIRT_GF_RTC_BASE */
		case 0x8003:   /* BI_VIRT_GF_TTY_BASE */
			break; /* don't care */

		case 0x8004: /* BI_VIRT_VIRTIO_BASE */
			bootinfo.virtio_base = item->data[0];
			break;

		case 0x8005: /* BI_VIRT_CTRL_BASE */
			bootinfo.virt_ctrl_base = item->data[0];
			break;

		default:
			kprintf("unknown bootinfo tag %hx\n", item->tag);
			break;
		}

		ptr += item->size;
	}
}

extern void *fb_base;

void
cstart(struct handover *handover)
{
	gftty_init();
	kprintf("Keyronex-lite/virt68k: " __DATE__ " " __TIME__ "\n");

	fb_base = (void *)handover->fb_base;

	/* set up initial threading structures */
	ncpus = 1;
	ki_cpu_init(curcpu(), &thread0);
	thread0.last_cpu = &bootstrap_cpu;
	thread0.state = kThreadStateRunning;
	thread0.timeslice = 5;
	ki_thread_common_init(&thread0, curcpu(), &kernel_process.kprocess,
	    "idle0");

	void intr_init(void);
	intr_init();

	parse_bootinfo(handover->bootinfo);

	kassert(memory_size != 0);
	vm_region_add(handover->bumped_end, memory_size - handover->bumped_end);

	gfrtc_init();
	char datestr[40];
	ke_format_time(gfrtc_get_time(), datestr, 40);
	kprintf("System startup beginning at %s GMT.\n", datestr);

	asm("movec %0, %%dtt0" ::"d"(0));
	asm("movec %0, %%itt0" ::"d"(0));

	gfrtc_oneshot(NS_PER_S / KERN_HZ);

	vmp_kernel_init();
	kmem_init();
	obj_init();

	ps_create_kernel_thread(&ex_init_thread, "ex_init", ex_init, NULL);
	ke_thread_resume(ex_init_thread);

	/* this is now the idle thread */
	for (;;) {
		asm("stop #0x2000");
	}
}
