#include <limine.h>
#include <stddef.h>
#include <stdint.h>

#include "amd64.h"
#include "vm/vm.h"
#include "ke/ke.h"

enum { kPortCOM1 = 0x3f8 };

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent.


volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

static volatile struct limine_hhdm_request hhdm_request = {
	.id = LIMINE_HHDM_REQUEST,
	.revision = 0
};

static volatile struct limine_kernel_address_request kernel_address_request = {
	.id = LIMINE_KERNEL_ADDRESS_REQUEST,
	.revision = 0
};

static volatile struct limine_kernel_file_request kernel_file_request = {
	.id = LIMINE_KERNEL_FILE_REQUEST,
	.revision = 0
};

static volatile struct limine_memmap_request memmap_request = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};

volatile struct limine_module_request module_request = {
	.id = LIMINE_MODULE_REQUEST,
	.revision = 0
};

volatile struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST,
	.revision = 0 };

static volatile struct limine_smp_request smp_request = {
	.id = LIMINE_SMP_REQUEST,
	.revision = 0
};

volatile struct limine_terminal_request terminal_request = {
	.id = LIMINE_TERMINAL_REQUEST,
	.revision = 0
};

static void
done(void)
{
	kdprintf("Done.\n");
	for (;;) {
		__asm__("hlt");
	}
}

const unsigned logow = 70, logoh = 22;
const unsigned logosize = logow * logoh * 4;
extern const char logosmall[6160];

void
draw_logo(void)
{
	struct limine_framebuffer *fbs =
	    terminal_request.response->terminals[0]->framebuffer;
	size_t fbw = fbs->width;
	uint8_t *fb = fbs->address;

#define fb_at(x, y) &fb[y * 4 * fbw + x * 4];
	size_t startx = 0, starty = 0;
	size_t x = startx, y = starty;
	for (unsigned i = 0; i < sizeof(logosmall); i += 4) {
		if (i % (logow * 4) == 0) {
			y++;
			x = startx;
		}

		uint8_t *fba = (uint8_t *)fb_at(x, y);
		uint8_t *pica = (uint8_t *)&logosmall[i];

		*fba++ = *(pica + 2);
		*fba++ = *(pica + 0);
		*fba++ = *(pica + 1);
		*fba++ = *(pica + 3);

		x++;
	}
}

static void
serial_init()
{
	outb(kPortCOM1 + 1, 0x00);
	outb(kPortCOM1 + 3, 0x80);
	outb(kPortCOM1 + 0, 0x03);
	outb(kPortCOM1 + 1, 0x00);
	outb(kPortCOM1 + 3, 0x03);
	outb(kPortCOM1 + 2, 0xC7);
	outb(kPortCOM1 + 4, 0x0B);
}


/* put character to limine terminal + COM1 */
void
hl_dputc(int ch, void *ctx)
{
	/* put to com1 */
	while (!(inb(kPortCOM1 + 5) & 0x20))
		;
	outb(kPortCOM1, ch);

	/* put to syscon/limine terminal */
	//if (!syscon) {
		struct limine_terminal *terminal =
		    terminal_request.response->terminals[0];
		terminal_request.response->write(terminal, (char *)&ch, 1);
	//} else {
	//	sysconputc(ch);
	//}
}

static void
mem_init()
{
	if (hhdm_request.response->offset != 0xffff800000000000) {
		/* we expect HHDM begins there for now for simplicity */
		kdprintf("Unexpected HHDM offset (assumes 0xffff800000000000, "
			"actual %lx",
		    hhdm_request.response->offset);
		done();
	}

	if (kernel_address_request.response->virtual_base !=
	    0xffffffff80000000) {
		kdprintf("Unexpected kernel virtual base %lx",
		    kernel_address_request.response->virtual_base);
		done();
	}

	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (int i = 0; i < memmap_request.response->entry_count; i++) {
		if (entries[i]->type != 0 || entries[i]->base < 0x100000)
			continue;

		vi_region_add(entries[i]->base, entries[i]->length);
	}
}

// The following will be our kernel's entry point.
void
_start(void)
{
	serial_init();

	// Ensure we got a terminal
	if (terminal_request.response == NULL ||
	    terminal_request.response->terminal_count < 1) {
		
		done();
	}

	draw_logo();

	kdprintf("Melantix (TM) Kernel Version 1.0-alpha\n");

	mem_init();

	// We're done, just hang...
	done();
}