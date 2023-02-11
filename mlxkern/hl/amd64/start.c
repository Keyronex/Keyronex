#include <limine.h>
#include <stddef.h>
#include <stdint.h>

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent.

static volatile struct limine_framebuffer_request framebuffer_request = {
	.id = LIMINE_FRAMEBUFFER_REQUEST,
	.revision = 0
};

static volatile struct limine_terminal_request terminal_request = {
	.id = LIMINE_TERMINAL_REQUEST,
	.revision = 0
};

static void
done(void)
{
	for (;;) {
		__asm__("hlt");
	}
}

const unsigned logow = 63, logoh = 20;
const unsigned logosize = logow * logoh * 4;
extern const char logosmall[5040];

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

// The following will be our kernel's entry point.
void
_start(void)
{
	// Ensure we got a terminal
	if (terminal_request.response == NULL ||
	    terminal_request.response->terminal_count < 1) {
		done();
	}

	// We should now be able to call the Limine terminal to print out
	// a simple "Hello World" to screen.
	struct limine_terminal *terminal =
	    terminal_request.response->terminals[0];
	terminal_request.response->write(terminal, "Hello Melantix World", 20);

	draw_logo();

	// We're done, just hang...
	done();
}