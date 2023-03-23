#include "../../vendor/limine-terminal/backends/framebuffer.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/machdep.h"
#include "kdk/vm.h"

#include "fbcons.hh"

extern char key272x70[76160];
extern char netascale102x82[33456];
const int logow = 272;

static void *
term_alloc(size_t size)
{
	return kmem_alloc(size);
}

void
draw(uint8_t *src, uint8_t *dst, size_t dstx, size_t dsty, size_t dstw,
    size_t w, size_t h)
{
	size_t x = dstx;
	for (unsigned i = 0; i < (w * h * 4); i += 4) {
		if (i % (w * 4) == 0) {
			dsty++;
			x = dstx;
		}

		uint8_t *fba = (uint8_t *)&dst[dsty * 4 * dstw + x * 4];
		uint8_t *pica = (uint8_t *)&src[i];

		*fba++ = *(pica + 2);
		*fba++ = *(pica + 0);
		*fba++ = *(pica + 1);
		*fba++ = *(pica + 3);

		x++;
	}
}

FBConsole::FBConsole()
{
	struct limine_framebuffer *fb;
	struct term_context *term;
	struct fbterm_context *gterm;
	uint32_t *bg_canvas;

	fb = framebuffer_request.response->framebuffers[0];
	bg_canvas = NULL; /*(uint32_t *)kmem_alloc(
	    fb->width * fb->height * sizeof(uint32_t));*/

	term = fbterm_init(term_alloc, (uint32_t *)fb->address, fb->width,
	    fb->height, fb->pitch, bg_canvas, NULL, NULL, NULL, NULL, NULL,
	    NULL, NULL, 0, 0, 0, 1, 1, 12, 98, 70);
	term_context_reinit(term);

	gterm = (fbterm_context *)term;

#define BGCANVAS(x, y)                               \
	((uint32_t *)fb->address)[(y)*gterm->width + \
	    (x)] // gterm->canvas[(y) * gterm->width + (x)]
#define BOX(X, Y, X2, Y2, COL)                    \
	for (uint64_t x = X; x < X2; x++)         \
		for (uint64_t y = Y; y < Y2; y++) \
	BGCANVAS(x, y) = COL
#define HLINE(y, xstart, xend, col)                   \
	for (int ani = (xstart); ani < (xend); ani++) \
		BGCANVAS(ani, y) = col;
#define VLINE(ystart, yend, x, col)                   \
	for (int ani = (ystart); ani < (yend); ani++) \
		BGCANVAS(x, ani) = col;

	/* upper white box */
	BOX(1, 1, fb->width, 86, 0xffffffff);
	/* purple border below white box */
	BOX(1, 87, fb->width, 91, 0x515191);
	/* grey box at bottom */
	BOX(1, fb->height - 64, fb->width, fb->height, 0xc3c3c3);

	draw((uint8_t *)key272x70, (uint8_t *)fb->address, 8, 8, fb->width, 272,
	    70);
	draw((uint8_t *)netascale102x82, (uint8_t *)fb->address,
	    fb->width - 108, 2, fb->width, 102, 82);

	for (int i = 0; i < 40; i++)
		term_write(term, "Hello World\n", strlen("Hello Warudo\n"));
}