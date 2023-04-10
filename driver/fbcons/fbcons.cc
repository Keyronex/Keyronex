/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include <termios.h>

#include "../../vendor/limine-terminal/backends/framebuffer.h"
#include "kdk/kmem.h"
#include "kdk/vm.h"

#include "fbcons.hh"

extern char key272x70[76160];
extern char netascale102x82[33456];

FBConsole *FBConsole::syscon = NULL;
static unsigned sequence_num = 0;

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

void
plot_char_abs(struct fbterm_context *gterm, struct fbterm_char *c, size_t x,
    size_t y)
{
	bool *glyph =
	    &gterm->font_bool[c->c * gterm->font_height * gterm->font_width];

	for (size_t gy = 0; gy < gterm->glyph_height; gy++) {
		uint8_t fy = gy / gterm->font_scale_y;
		volatile uint32_t *fb_line = gterm->framebuffer + x +
		    (y + gy) * (gterm->pitch / 4);
		uint32_t *canvas_line = gterm->canvas + x +
		    (y + gy) * gterm->width;
		for (size_t fx = 0; fx < gterm->font_width; fx++) {
			bool draw = glyph[fy * gterm->font_width + fx];
			for (size_t i = 0; i < gterm->font_scale_x; i++) {
				size_t gx = gterm->font_scale_x * fx + i;
				uint32_t bg = c->bg == 0xFFFFFFFF ?
				    canvas_line[gx] :
				    c->bg;
				uint32_t fg = c->fg == 0xFFFFFFFF ?
				    canvas_line[gx] :
				    c->fg;
				fb_line[gx] = draw ? fg : bg;
			}
		}
	}
}

void
FBConsole::puts(const char *buf, size_t len)
{
	term_write(syscon->term, buf, len);
}

void
FBConsole::printstats()
{
	struct fbterm_context *gterm = (fbterm_context *)syscon->term;
	struct pctx {
		struct fbterm_context *gterm;
		unsigned x, y;
	};

	auto putc = [](int ch, void *ctx) {
		pctx *pctx = (struct pctx *)ctx;
		struct fbterm_char chr = { .c = (uint32_t)ch,
			.fg = 0x030303,
			.bg = 0xc3c3c3 };
		plot_char_abs(pctx->gterm, &chr, 8 + pctx->x,
		    syscon->fb->height - 32 + pctx->y);
		pctx->x += pctx->gterm->font_width;
	};

#define STPRINT(X, Y, ...)                                      \
	({                                                      \
		pctx pctx = { .gterm = gterm, .x = X, .y = Y }; \
		npf_pprintf(putc, &pctx, __VA_ARGS__);          \
	})
	STPRINT(0, 0,
	    "FREE %lu; ACT %lu; INACT %lu; WIRE %lu; VMM %lu; DEV %lu",
	    vmstat.nfree, vmstat.nactive, vmstat.ninactive,
	    vmstat.nwired + vmstat.npermwired, vmstat.nvmm, vmstat.ndev);
	STPRINT(0, 16, "OBJ: %lu; ANON: %lu | Console: %s", vmstat.nobject,
	    vmstat.nanon, syscon->objhdr.name);
}

void
FBConsole::getsize(struct winsize *ws)
{
	ws->ws_col = syscon->term->cols;
	ws->ws_row = syscon->term->rows;
	ws->ws_xpixel = syscon->fb->width - 8 * 2;
	ws->ws_ypixel = syscon->fb->height - 93 - 36;
}

FBConsole::FBConsole(Device *provider)
{
	struct fbterm_context *gterm;
	unsigned num;

	fb = framebuffer_request.response->framebuffers[0];

	term = fbterm_init(term_alloc, (uint32_t *)fb->address, fb->width,
	    fb->height, fb->pitch, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	    NULL, 0, 0, 0, 1, 1, 8, 93, 36);
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
	BOX(1, 87, fb->width, 91, 0x6f6fa3);
	/* grey box at bottom */
	BOX(1, fb->height - 32, fb->width, fb->height, 0xc3c3c3);

	draw((uint8_t *)key272x70, (uint8_t *)fb->address, 8, 8, fb->width, 272,
	    70);
	draw((uint8_t *)netascale102x82, (uint8_t *)fb->address,
	    fb->width - 108, 2, fb->width, 102, 82);

	num = sequence_num++;

	if (num == 0) {
		syscon = this;
		syscon_puts = puts;
		syscon_printstats = printstats;
		syscon_getsize = getsize;
		hl_replaykmsgbuf();
	}

	kmem_asprintf(&objhdr.name, "fbcons%u", num);
	attach(provider);

	DKDevLog(this, "%lux%lux%d\n", fb->width, fb->height, fb->bpp);
}