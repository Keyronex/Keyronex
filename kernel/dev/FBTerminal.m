#include "../limine-terminal/backends/framebuffer.h"
#include "dev/FBTerminal.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

#define PROVIDER ((DKFramebuffer *)m_provider);

extern char key272x70[76160];
extern char netascale102x82[33456];

FBTerminal *system_terminal = nil;
static int counter = 0;

@interface
FBTerminal (Local)
- (instancetype)initWithFramebuffer:(DKFramebuffer *)framebuffer;
@end

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

		*fba++ = *(pica + 3);
		*fba++ = *(pica + 0);
		*fba++ = *(pica + 1);
		*fba++ = *(pica + 2);

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

struct pctx {
	struct fbterm_context *gterm;
	unsigned x, y;
};

static void
stat_putc(int ch, void *ctx)
{
	struct pctx *pctx = (struct pctx *)ctx;
	struct fbterm_char chr = { .c = (uint32_t)ch,
		.fg = 0x030303,
		.bg = 0xc3c3c3 };
	plot_char_abs(pctx->gterm, &chr, 8 + pctx->x,
	    pctx->gterm->height - 32 + pctx->y);
	pctx->x += pctx->gterm->font_width;
}

static void
fbterminal_printstats()
{
	struct fbterm_context *gterm = (void *)system_terminal->term;

#define STPRINT(X, Y, ...)                                             \
	({                                                             \
		struct pctx pctx = { .gterm = gterm, .x = X, .y = Y }; \
		npf_pprintf(stat_putc, &pctx, __VA_ARGS__);            \
	})
	STPRINT(0, 0, "ACT: %zu MOD: %zu STBY: %zu FREE: %zu", vmstat.nactive, vmstat.nmodified, vmstat.nstandby, vmstat.nfree);
	STPRINT(0, 16, "Console: %s", obj_name(system_terminal));
}

@implementation FBTerminal

+ (BOOL)probeWithFramebuffer:(DKFramebuffer *)framebuffer
{
	[[self alloc] initWithFramebuffer:framebuffer];
	return YES;
}

- (instancetype)initWithFramebuffer:(DKFramebuffer *)framebuffer
{
	struct fbterm_context *gterm;
	uint32_t *address;

	self = [super initWithProvider:framebuffer];

	kmem_asprintf(obj_name_ptr(self), "fbterminal-%d", counter++);

	address = (void*)P2V(framebuffer.info.address);
	term = fbterm_init(term_alloc, address, framebuffer.info.width,
	    framebuffer.info.height, framebuffer.info.pitch, NULL, NULL, NULL,
	    NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 1, 1, 8, 93, 36);
	gterm = (struct fbterm_context *)term;

#define BGCANVAS(x, y)               \
	(address)[(y)*gterm->width + \
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
	BOX(1, 1, gterm->width, 86, 0xffffffff);
	/* purple border below white box */
	BOX(1, 87, gterm->width, 91, 0x6f6fa3);
	/* grey box at bottom */
	BOX(1, gterm->height - 32, gterm->width, gterm->height, 0xc3c3c3);

	draw((uint8_t *)key272x70, (uint8_t *)address, 8, 8, gterm->width, 272,
	    70);
	draw((uint8_t *)netascale102x82, (uint8_t *)address, gterm->width - 108,
	    2, gterm->width, 102, 82);

	system_terminal = self;

	extern void ki_replay_msgbuf(void);
	ki_replay_msgbuf();
	fbterminal_printstats();

	[self registerDevice];

	return self;
}

@end

void
fbterminal_putc(FBTerminal *term, int ch)
{
	char c = ch;
	term_write(term->term, &c, 1);
}
