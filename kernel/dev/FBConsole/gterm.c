#include <string.h>
#include "image.h"
#include "gterm.h"
#include "term.h"

typedef struct {
	unsigned char magic[4];
	unsigned int version;
	unsigned int headersize;
	unsigned int flags;
	unsigned int nglyph;
	unsigned int nbyteperglyph;
	unsigned int height;
	unsigned int width;
}psf_font_header_t;

static uint64_t sqrt(uint64_t a_nInput)
{
    uint64_t op  = a_nInput;
    uint64_t res = 0;
    uint64_t one = 1UL << 62;

    while (one > op) one >>= 2;

    while (one != 0)
    {
        if (op >= res + one)
        {
            op = op - (res + one);
            res = res +  2 * one;
        }
        res >>= 1;
        one >>= 2;
    }

    return res;
}

void gterm_save_state(struct gterm_t *gterm)
{
    gterm->context.saved_state_text_fg = gterm->context.text_fg;
    gterm->context.saved_state_text_bg = gterm->context.text_bg;
    gterm->context.saved_state_cursor_x = gterm->context.cursor_x;
    gterm->context.saved_state_cursor_y = gterm->context.cursor_y;
}

void gterm_restore_state(struct gterm_t *gterm)
{
    gterm->context.text_fg = gterm->context.saved_state_text_fg;
    gterm->context.text_bg = gterm->context.saved_state_text_bg;
    gterm->context.cursor_x = gterm->context.saved_state_cursor_x;
    gterm->context.cursor_y = gterm->context.saved_state_cursor_y;
}

void gterm_swap_palette(struct gterm_t *gterm)
{
    uint32_t tmp = gterm->context.text_bg;
    gterm->context.text_bg = gterm->context.text_fg;
    gterm->context.text_fg = tmp;
}

#define A(rgb) (uint8_t)(rgb >> 24)
#define R(rgb) (uint8_t)(rgb >> 16)
#define G(rgb) (uint8_t)(rgb >> 8)
#define B(rgb) (uint8_t)(rgb)
#define ARGB(a, r, g, b) ((a << 24) | ((uint8_t)r << 16) | ((uint8_t)g << 8) | (uint8_t)b)

static uint32_t colour_blend(struct gterm_t *gterm, uint32_t fg, uint32_t bg)
{
    (void)gterm;
    unsigned alpha = 255 - A(fg);
    unsigned inv_alpha = A(fg) + 1;

    uint8_t r = (uint8_t)((alpha * R(fg) + inv_alpha * R(bg)) / 256);
    uint8_t g = (uint8_t)((alpha * G(fg) + inv_alpha * G(bg)) / 256);
    uint8_t b = (uint8_t)((alpha * B(fg) + inv_alpha * B(bg)) / 256);

    return ARGB(0, r, g, b);
}

static void plot_px(struct gterm_t *gterm, size_t x, size_t y, uint32_t hex)
{
    if (x >= gterm->framebuffer.width || y >= gterm->framebuffer.height) return;

    size_t fb_i = x + (gterm->framebuffer.pitch / sizeof(uint32_t)) * y;

    gterm->framebuffer_addr[fb_i] = hex;
}

__attribute__((no_sanitize("undefined")))
static uint32_t blend_gradient_from_box(struct gterm_t *gterm, size_t x, size_t y, uint32_t bg_px, uint32_t hex)
{
    size_t distance, x_distance, y_distance;
    size_t gradient_stop_x = gterm->framebuffer.width - gterm->margin;
    size_t gradient_stop_y = gterm->framebuffer.height - gterm->margin;

    if (x < gterm->margin) x_distance = gterm->margin - x;
    else x_distance = x - gradient_stop_x;

    if (y < gterm->margin) y_distance = gterm->margin - y;
    else y_distance = y - gradient_stop_y;

    if (x >= gterm->margin && x < gradient_stop_x) distance = y_distance;
    else if (y >= gterm->margin && y < gradient_stop_y) distance = x_distance;
    else distance = sqrt((uint64_t)x_distance * (uint64_t)x_distance + (uint64_t)y_distance * (uint64_t)y_distance);

    if (distance > gterm->margin_gradient) return bg_px;

    uint8_t gradient_step = (0xff - A(hex)) / gterm->margin_gradient;
    uint8_t new_alpha = A(hex) + gradient_step * distance;

    return colour_blend(gterm, (hex & 0xFFFFFF) | (new_alpha << 24), bg_px);
}

__attribute__((no_sanitize("undefined")))
static void genloop(struct gterm_t *gterm, size_t xstart, size_t xend, size_t ystart, size_t yend, uint32_t (*blend)(struct gterm_t *gterm, size_t x, size_t y, uint32_t orig))
{
    uint8_t *img = gterm->background->img;
    const size_t img_width = gterm->background->img_width, img_height = gterm->background->img_height, img_pitch = gterm->background->pitch, colsize = gterm->background->bpp / 8;

    switch (gterm->background->type)
    {
        case TILED:
            for (size_t y = ystart; y < yend; y++)
            {
                size_t image_y = y % img_height, image_x = xstart % img_width;
                const size_t off = img_pitch * (img_height - 1 - image_y);
                size_t canvas_off = gterm->framebuffer.width * y, fb_off = gterm->framebuffer.pitch / 4 * y;
                for (size_t x = xstart; x < xend; x++)
                {
                    uint32_t img_pixel = *((uint32_t*)(img + image_x * colsize + off));
                    uint32_t i = blend(gterm, x, y, img_pixel);
                    gterm->bg_canvas[canvas_off + x] = i;
                    gterm->framebuffer_addr[fb_off + x] = i;
                    if (image_x++ == img_width) image_x = 0;
                }
            }
            break;

        case CENTERED:
            for (size_t y = ystart; y < yend; y++)
            {
                size_t image_y = y - gterm->background->y_displacement;
                const size_t off = img_pitch * (img_height - 1 - image_y);
                size_t canvas_off = gterm->framebuffer.width * y, fb_off = gterm->framebuffer.pitch / 4 * y;
                if (image_y >= gterm->background->y_size)
                {
                    for (size_t x = xstart; x < xend; x++)
                    {
                        uint32_t i = blend(gterm, x, y, gterm->background->back_colour);
                        gterm->bg_canvas[canvas_off + x] = i;
                        gterm->framebuffer_addr[fb_off + x] = i;
                    }
                }
                else
                {
                    for (size_t x = xstart; x < xend; x++)
                    {
                        size_t image_x = (x - gterm->background->x_displacement);
                        bool x_external = image_x >= gterm->background->x_size;
                        uint32_t img_pixel = *((uint32_t*)(img + image_x * colsize + off));
                        uint32_t i = blend(gterm, x, y, x_external ? gterm->background->back_colour : img_pixel);
                        gterm->bg_canvas[canvas_off + x] = i;
                        gterm->framebuffer_addr[fb_off + x] = i;
                    }
                }
            }
            break;
        case STRETCHED:
            for (size_t y = ystart; y < yend; y++)
            {
                size_t img_y = (y * img_height) / gterm->framebuffer.height;
                size_t off = img_pitch * (img_height - 1 - img_y);
                size_t canvas_off = gterm->framebuffer.width * y, fb_off = gterm->framebuffer.pitch / 4 * y;

                size_t ratio = int_to_fixedp6(img_width) / gterm->framebuffer.width;
                fixedp6 img_x = ratio * xstart;
                for (size_t x = xstart; x < xend; x++)
                {
                    uint32_t img_pixel = *((uint32_t*)(img + fixedp6_to_int(img_x) * colsize + off));
                    uint32_t i = blend(gterm, x, y, img_pixel);
                    gterm->bg_canvas[canvas_off + x] = i; gterm->framebuffer_addr[fb_off + x] = i;
                    img_x += ratio;
                }
            }
            break;
    }
}

static uint32_t blend_external(struct gterm_t *gterm, size_t x, size_t y, uint32_t orig)
{
    (void)gterm;
    (void)x;
    (void)y;
    return orig;
}
static uint32_t blend_internal(struct gterm_t *gterm, size_t x, size_t y, uint32_t orig)
{
    (void)x;
    (void)y;
    return colour_blend(gterm, gterm->default_bg, orig);
}
static uint32_t blend_margin(struct gterm_t *gterm, size_t x, size_t y, uint32_t orig)
{
    return blend_gradient_from_box(gterm, x, y, orig, gterm->default_bg);
}

static void loop_external(struct gterm_t *gterm, size_t xstart, size_t xend, size_t ystart, size_t yend)
{
    genloop(gterm, xstart, xend, ystart, yend, blend_external);
}
static void loop_margin(struct gterm_t *gterm, size_t xstart, size_t xend, size_t ystart, size_t yend)
{
    //genloop(gterm, xstart, xend, ystart, yend, blend_margin);
}
static void loop_internal(struct gterm_t *gterm, size_t xstart, size_t xend, size_t ystart, size_t yend)
{
    genloop(gterm, xstart, xend, ystart, yend, blend_internal);
}



static void generate_canvas(struct gterm_t *gterm)
{
    int bgcol = 0x00666699;
    int margin = 40;
    int titlebar_start_y = 40;
    int titlebar_end_y = titlebar_start_y + 25;
    int titlebar_start_x = 40;
    int titlebar_end_x = gterm->framebuffer.width - margin;

    #define BGCANVAS(x, y) gterm->bg_canvas[(y) * gterm->framebuffer.width + (x)]
    #define HLINE(y, xstart, xend, col) for (int ani = (xstart); ani < (xend); ani++) \
        BGCANVAS(ani, y) = col;
    #define VLINE(ystart, yend, x, col) for (int ani = (ystart); ani < (yend); ani++) \
        BGCANVAS(x, ani) = col;

    for (size_t y = 0; y < gterm->framebuffer.height; y++)
    {
        for (size_t x = 0; x < gterm->framebuffer.width; x++)
        {
            gterm->bg_canvas[y * gterm->framebuffer.width + x] = bgcol;
        }
    }

    /* black line top titlebar */
    HLINE(titlebar_start_y, margin, titlebar_end_x, 0x00000000);
    /* white line top titlebar */
    HLINE(titlebar_start_y + 1, margin, titlebar_end_x - 1, 0xffffffff);
    /* grey line bottom titlebar */
    HLINE(titlebar_start_y + 23, margin + 2, titlebar_end_x - 1, 0x575757);
    /* black line bottom titlebar */
    HLINE(titlebar_start_y + 24, margin, titlebar_end_x, 0x00000000);
    /* black linebottom of window */
    HLINE(gterm->framebuffer.height - margin, titlebar_start_x, titlebar_end_x, 0x000000000);
    /* grey linebottom of window */
    HLINE(1 + gterm->framebuffer.height - margin, titlebar_start_x + 1, titlebar_end_x + 1, 0x00575757);

    /* white line left titlebar */
    VLINE(titlebar_start_y + 1, titlebar_start_y + 24, titlebar_start_x + 1, 0xffffffff);
    /* grey line right titlebar */
    VLINE(titlebar_start_y + 1, titlebar_start_y + 24, titlebar_end_x - 1, 0x575757);

    /* black line left window */
    VLINE(titlebar_start_y, gterm->framebuffer.height - margin, titlebar_start_x, 0x0);
    /* black line right window */
    VLINE(titlebar_start_y, 1 + gterm->framebuffer.height - margin, gterm->framebuffer.width - margin, 0x0);
    /* gray shadow line right window */
    VLINE(titlebar_start_y + 1, 2 + gterm->framebuffer.height - margin, 1 + gterm->framebuffer.width - margin, 0x00575757);

    /* fill titlebar grey */
    for (int i = titlebar_start_y + 2 ; i < titlebar_end_y - 2; i++)
        HLINE(i, titlebar_start_x + 2, titlebar_end_x - 1, 0x00b8b8b8);

    /* fill white background */
    for (int i = titlebar_end_y ; i < gterm->framebuffer.height - margin; i++)
        HLINE(i, titlebar_start_x + 1, titlebar_end_x, 0x00DDDDDD);

    for (size_t y = 0; y < gterm->framebuffer.height; y++)
    {
        for (size_t x = 0; x < gterm->framebuffer.width; x++)
        {
            plot_px(gterm, x, y, gterm->bg_canvas[y * gterm->framebuffer.width + x]);
        }
    }

    char text[] = "Keyronex";
    int startx = (gterm->framebuffer.width / 2) - ((gterm->glyph_width + 1) * strlen(text) / 2);
    void plot_char_abs(struct gterm_t *gterm, struct gterm_char *c, size_t x, size_t y);

    for (int i = 0 ; i < strlen(text); i++) {
        struct gterm_char chr = {
            .bg = 0xffffffff,
            .fg = 0x0,
            .c = text[i]
        };
        plot_char_abs(gterm, &chr, startx, titlebar_start_y + 6);
        startx += gterm->glyph_width + 1;
    }
}

void plot_char_abs(struct gterm_t *gterm, struct gterm_char *c, size_t x, size_t y)
{
    bool *glyph = &gterm->vga_font_bool[c->c * gterm->vga_font_height * gterm->vga_font_width];

    for (size_t gy = 0; gy < gterm->glyph_height; gy++)
    {
        uint8_t fy = gy / gterm->vga_font_scale_y;
        volatile uint32_t *fb_line = gterm->framebuffer_addr + x + (y + gy) * (gterm->framebuffer.pitch / 4);
        uint32_t *canvas_line = gterm->bg_canvas + x + (y + gy) * gterm->framebuffer.width;
        for (size_t fx = 0; fx < gterm->vga_font_width; fx++)
        {
            bool draw = glyph[fy * gterm->vga_font_width + fx];
            for (size_t i = 0; i < gterm->vga_font_scale_x; i++)
            {
                size_t gx = gterm->vga_font_scale_x * fx + i;
                uint32_t bg = c->bg == 0xFFFFFFFF ? canvas_line[gx] : c->bg;
                uint32_t fg = c->fg == 0xFFFFFFFF ? canvas_line[gx] : c->fg;
                fb_line[gx] = draw ? fg : bg;
            }
        }
    }
}

static void plot_char(struct gterm_t *gterm, struct gterm_char *c, size_t x, size_t y)
{
    if (x >= gterm->cols || y >= gterm->rows) return;

    x = gterm->offset_x + x * gterm->glyph_width;
    y = gterm->offset_y + y * gterm->glyph_height;

    plot_char_abs(gterm, c, x, y);
}

static void plot_char_fast(struct gterm_t *gterm, struct gterm_char *old, struct gterm_char *c, size_t x, size_t y)
{
    if (x >= gterm->cols || y >= gterm->rows) return;

    x = gterm->offset_x + x * gterm->glyph_width;
    y = gterm->offset_y + y * gterm->glyph_height;

    bool *new_glyph = &gterm->vga_font_bool[c->c * gterm->vga_font_height * gterm->vga_font_width];
    bool *old_glyph = &gterm->vga_font_bool[old->c * gterm->vga_font_height * gterm->vga_font_width];
    for (size_t gy = 0; gy < gterm->glyph_height; gy++)
    {
        uint8_t fy = gy / gterm->vga_font_scale_y;
        volatile uint32_t *fb_line = gterm->framebuffer_addr + x + (y + gy) * (gterm->framebuffer.pitch / 4);
        uint32_t *canvas_line = gterm->bg_canvas + x + (y + gy) * gterm->framebuffer.width;
        for (size_t fx = 0; fx < gterm->vga_font_width; fx++)
        {
            bool old_draw = old_glyph[fy * gterm->vga_font_width + fx];
            bool new_draw = new_glyph[fy * gterm->vga_font_width + fx];
            if (old_draw == new_draw) continue;

            for (size_t i = 0; i < gterm->vga_font_scale_x; i++)
            {
                size_t gx = gterm->vga_font_scale_x * fx + i;
                uint32_t bg = c->bg == 0xFFFFFFFF ? canvas_line[gx] : c->bg;
                uint32_t fg = c->fg == 0xFFFFFFFF ? canvas_line[gx] : c->fg;
                fb_line[gx] = new_draw ? fg : bg;
            }
        }
    }
}

static bool compare_char(struct gterm_char *a, struct gterm_char *b)
{
    return !(a->c != b->c || a->bg != b->bg || a->fg != b->fg);
}

static void push_to_queue(struct gterm_t *gterm, struct gterm_char *c, size_t x, size_t y)
{
    if (x >= gterm->cols || y >= gterm->rows) return;

    size_t i = y * gterm->cols + x;

    struct gterm_queue_item *q = gterm->map[i];

    if (!q)
    {
        if (compare_char(&gterm->grid[i], c)) return;
        q = &gterm->queue[gterm->queue_i++];
        q->x = x;
        q->y = y;
        gterm->map[i] = q;
    }

    q->c = *c;
}

bool gterm_scroll_disable(struct gterm_t *gterm)
{
    bool ret = gterm->context.scroll_enabled;
    gterm->context.scroll_enabled = false;
    return ret;
}

void gterm_scroll_enable(struct gterm_t *gterm)
{
    gterm->context.scroll_enabled = true;
}

void gterm_revscroll(struct gterm_t *gterm)
{
    for (size_t i = (gterm->term->context.scroll_bottom_margin - 1) * gterm->cols - 1; ; i--)
    {
        struct gterm_char *c;
        struct gterm_queue_item *q = gterm->map[i];
        if (q) c = &q->c;
        else c = &gterm->grid[i];

        push_to_queue(gterm, c, (i + gterm->cols) % gterm->cols, (i + gterm->cols) / gterm->cols);
        if (i == gterm->term->context.scroll_top_margin * gterm->cols) break;
    }

    struct gterm_char empty;
    empty.c  = ' ';
    empty.fg = gterm->context.text_fg;
    empty.bg = gterm->context.text_bg;

    for (size_t i = gterm->term->context.scroll_top_margin * gterm->cols; i < (gterm->term->context.scroll_top_margin + 1) * gterm->cols; i++)
    {
        push_to_queue(gterm, &empty, i % gterm->cols, i / gterm->cols);
    }
}

void gterm_scroll(struct gterm_t *gterm)
{
    for (size_t i = (gterm->term->context.scroll_top_margin + 1) * gterm->cols; i < gterm->term->context.scroll_bottom_margin * gterm->cols; i++)
    {
        struct gterm_char *c;
        struct gterm_queue_item *q = gterm->map[i];
        if (q) c = &q->c;
        else c = &gterm->grid[i];
        push_to_queue(gterm, c, (i - gterm->cols) % gterm->cols, (i - gterm->cols) / gterm->cols);
    }

    struct gterm_char empty;
    empty.c  = ' ';
    empty.fg = gterm->context.text_fg;
    empty.bg = gterm->context.text_bg;
    for (size_t i = (gterm->term->context.scroll_bottom_margin - 1) * gterm->cols; i < gterm->term->context.scroll_bottom_margin * gterm->cols; i++)
    {
        push_to_queue(gterm, &empty, i % gterm->cols, i / gterm->cols);
    }
}

void gterm_clear(struct gterm_t *gterm, bool move)
{
    struct gterm_char empty;
    empty.c  = ' ';
    empty.fg = gterm->context.text_fg;
    empty.bg = gterm->context.text_bg;
    for (size_t i = 0; i < gterm->rows * gterm->cols; i++)
    {
        push_to_queue(gterm, &empty, i % gterm->cols, i / gterm->cols);
    }

    if (move)
    {
        gterm->context.cursor_x = 0;
        gterm->context.cursor_y = 0;
    }
}

void gterm_enable_cursor(struct gterm_t *gterm)
{
    gterm->context.cursor_status = true;
}

bool gterm_disable_cursor(struct gterm_t *gterm)
{
    bool ret = gterm->context.cursor_status;
    gterm->context.cursor_status = false;
    return ret;
}

void gterm_set_cursor_pos(struct gterm_t *gterm, size_t x, size_t y)
{
    if (x >= gterm->cols)
    {
        if ((int)x < 0) x = 0;
        else x = gterm->cols - 1;
    }
    if (y >= gterm->rows)
    {
        if ((int)y < 0) y = 0;
        else y = gterm->rows - 1;
    }
    gterm->context.cursor_x = x;
    gterm->context.cursor_y = y;
}

void gterm_get_cursor_pos(struct gterm_t *gterm, size_t *x, size_t *y)
{
    *x = gterm->context.cursor_x;
    *y = gterm->context.cursor_y;
}

void gterm_move_character(struct gterm_t *gterm, size_t new_x, size_t new_y, size_t old_x, size_t old_y)
{
    if (old_x >= gterm->cols || old_y >= gterm->rows || new_x >= gterm->cols || new_y >= gterm->rows) return;

    size_t i = old_x + old_y * gterm->cols;

    struct gterm_char *c;
    struct gterm_queue_item *q = gterm->map[i];
    if (q) c = &q->c;
    else c = &gterm->grid[i];

    push_to_queue(gterm, c, new_x, new_y);
}

void gterm_set_text_fg(struct gterm_t *gterm, size_t fg)
{
    gterm->context.text_fg = gterm->ansi_colours[fg];
}

void gterm_set_text_bg(struct gterm_t *gterm, size_t bg)
{
    gterm->context.text_bg = gterm->ansi_colours[bg];
}

void gterm_set_text_fg_bright(struct gterm_t *gterm, size_t fg)
{
    gterm->context.text_fg = gterm->ansi_bright_colours[fg];
}

void gterm_set_text_bg_bright(struct gterm_t *gterm, size_t bg)
{
    gterm->context.text_bg = gterm->ansi_bright_colours[bg];
}

void gterm_set_text_fg_default(struct gterm_t *gterm)
{
    gterm->context.text_fg = gterm->default_fg;
}

void gterm_set_text_bg_default(struct gterm_t *gterm)
{
    gterm->context.text_bg = 0xFFFFFFFF;
}

static void draw_cursor(struct gterm_t *gterm)
{
    size_t i = gterm->context.cursor_x + gterm->context.cursor_y * gterm->cols;
    struct gterm_char c;
    struct gterm_queue_item *q = gterm->map[i];
    if (q) c = q->c;
    else c = gterm->grid[i];

    uint32_t tmp = c.fg;
    c.fg = c.bg;
    c.bg = tmp;
    plot_char(gterm, &c, gterm->context.cursor_x, gterm->context.cursor_y);
    if (q)
    {
        gterm->grid[i] = q->c;
        gterm->map[i] = NULL;
    }
}

void gterm_double_buffer_flush(struct gterm_t *gterm)
{
    if (gterm->context.cursor_status) draw_cursor(gterm);

    for (size_t i = 0; i < gterm->queue_i; i++)
    {
        struct gterm_queue_item *q = &gterm->queue[i];
        size_t offset = q->y * gterm->cols + q->x;
        if (gterm->map[offset] == NULL) continue;

        struct gterm_char *old = &gterm->grid[offset];
        if (q->c.bg == old->bg && q->c.fg == old->fg)
        {
            plot_char_fast(gterm, old, &q->c, q->x, q->y);
        }
        else plot_char(gterm, &q->c, q->x, q->y);

        gterm->grid[offset] = q->c;
        gterm->map[offset] = NULL;
    }

    if ((gterm->old_cursor_x != gterm->context.cursor_x || gterm->old_cursor_y != gterm->context.cursor_y) || gterm->context.cursor_status == false)
    {
        plot_char(gterm, &gterm->grid[gterm->old_cursor_x + gterm->old_cursor_y * gterm->cols], gterm->old_cursor_x, gterm->old_cursor_y);
    }

    gterm->old_cursor_x = gterm->context.cursor_x;
    gterm->old_cursor_y = gterm->context.cursor_y;

    gterm->queue_i = 0;
}

void gterm_putchar(struct gterm_t *gterm, uint8_t c)
{
    struct gterm_char ch;
    ch.c = c;
    ch.fg = gterm->context.text_fg;
    ch.bg = gterm->context.text_bg;
    push_to_queue(gterm, &ch, gterm->context.cursor_x++, gterm->context.cursor_y);
    if (gterm->context.cursor_x == gterm->cols && (gterm->context.cursor_y < gterm->term->context.scroll_bottom_margin - 1 || gterm->context.scroll_enabled))
    {
        gterm->context.cursor_x = 0;
        gterm->context.cursor_y++;
    }
    if (gterm->context.cursor_y == gterm->term->context.scroll_bottom_margin)
    {
        gterm->context.cursor_y--;
        gterm_scroll(gterm);
    }
}

bool gterm_init(struct gterm_t *gterm, struct term_t *term, struct framebuffer_t frm, struct font_t font, struct style_t style, struct background_t back)
{
    if (font.address == 0) return false;

    gterm->term = term;
    gterm->framebuffer = frm;
    gterm->framebuffer_addr = (volatile uint32_t*)frm.address;

    gterm->context.cursor_status = true;
    gterm->context.scroll_enabled = true;

    gterm->margin = 64;
    gterm->margin_top = 65;
    gterm->margin_gradient = 4;

    memcpy(gterm->ansi_colours, style.ansi_colours, 32);
    memcpy(gterm->ansi_bright_colours, style.ansi_bright_colours, 32);

    gterm->default_bg = style.background;
    gterm->default_fg = style.foreground & 0xFFFFFF;

    gterm->context.text_fg = gterm->default_fg;
    gterm->context.text_bg = 0xFFFFFFFF;

    gterm->background = back.background;

    if (gterm->background == NULL)
    {
        gterm->margin = 0;
        gterm->margin_gradient = 0;
    }
    else if (gterm->default_bg == DEFAULT_BACKGROUND) gterm->default_bg = 0x68000000;

    if (style.margin != (uint16_t)(-1)) gterm->margin = style.margin;
    if (style.margin_gradient != (uint16_t)(-1)) gterm->margin_gradient = style.margin_gradient;

#if 0
    if (gterm->background != NULL)
    {
        if (back.style == CENTERED)
        {
            image_make_centered(gterm->background, gterm->framebuffer.width, gterm->framebuffer.height, back.backdrop);
        }
        else if (back.style == STRETCHED)
        {
            image_make_stretched(gterm->background, gterm->framebuffer.width, gterm->framebuffer.height);
        }
    }
#endif

    psf_font_header_t *head = (psf_font_header_t*)font.address;
    uint8_t *glyphs = (uint8_t*)font.address + head->headersize;

    gterm->vga_font_width = head->width;
    gterm->vga_font_height = head->height;

    gterm->font_bytes = (gterm->vga_font_width * gterm->vga_font_height * VGA_FONT_GLYPHS) / 8;

    gterm->vga_font_width += font.spacing;

    gterm->vga_font_bool_size = VGA_FONT_GLYPHS * gterm->vga_font_height * gterm->vga_font_width * sizeof(bool);
    gterm->vga_font_bool = alloc_mem(gterm->vga_font_bool_size);

    for (size_t i = 0; i < VGA_FONT_GLYPHS; i++)
    {
        uint8_t *glyph =  &glyphs[i * head->nbyteperglyph];

        for (size_t y = 0; y < head->height; y++) {
            for (size_t x = 0; x < head->width; x++)
            {
                uint8_t bit = glyph[y * (head->nbyteperglyph / head->height) + x / 8] >> (7 - x % 8) & 1;
                gterm->vga_font_bool[i * gterm->vga_font_height * gterm->vga_font_width + y * gterm->vga_font_width + x] = bit ;
            }
        }
    }

#if 0
        uint8_t *glyph = &gterm->vga_font_bits[i * gterm->vga_font_height * 2];

        for (size_t y = 0; y < gterm->vga_font_height; y++)
        {
            for (size_t x = 0; x < 8; x++)
            {
                size_t offset = i * gterm->vga_font_height * gterm->vga_font_width + y * gterm->vga_font_width + x;

                if ((glyph[y] & (0x80 >> x))) gterm->vga_font_bool[offset] = true;
                else gterm->vga_font_bool[offset] = false;
            }

            for (size_t x = 8; x < gterm->vga_font_width; x++)
            {
                size_t offset = i * gterm->vga_font_height * gterm->vga_font_width + y * gterm->vga_font_width + x;

                if (i >= 0xC0 && i <= 0xDF) gterm->vga_font_bool[offset] = (glyph[y] & 1);
                else gterm->vga_font_bool[offset] = false;
            }
        }
#endif

    gterm->vga_font_scale_x = 1;
    gterm->vga_font_scale_y = 1;

    if (font.scale_x || font.scale_y)
    {
        gterm->vga_font_scale_x = font.scale_x;
        gterm->vga_font_scale_y = font.scale_y;
        if (gterm->vga_font_scale_x > 8 || gterm->vga_font_scale_y > 8)
        {
            gterm->vga_font_scale_x = 1;
            gterm->vga_font_scale_y = 1;
        }
    }

    gterm->glyph_width = gterm->vga_font_width * gterm->vga_font_scale_x;
    gterm->glyph_height = gterm->vga_font_height * gterm->vga_font_scale_y;

    gterm->cols = term->cols = (gterm->framebuffer.width - 8 - gterm->margin * 2) / gterm->glyph_width;
    gterm->rows = term->rows = (gterm->framebuffer.height - 8 - gterm->margin - gterm->margin_top) / gterm->glyph_height;

    gterm->offset_x = gterm->margin + ((gterm->framebuffer.width - gterm->margin * 2) % gterm->glyph_width) / 2 + 4;
    gterm->offset_y = gterm->margin_top + ((gterm->framebuffer.height - gterm->margin - gterm->margin_top) % gterm->glyph_height) / 2 + 4;

    gterm->grid_size = gterm->rows * gterm->cols * sizeof(struct gterm_char);
    gterm->grid = alloc_mem(gterm->grid_size);

    gterm->queue_size = gterm->rows * gterm->cols * sizeof(struct gterm_queue_item);
    gterm->queue = alloc_mem(gterm->queue_size);
    gterm->queue_i = 0;

    gterm->map_size = gterm->rows * gterm->cols * sizeof(struct gterm_queue_item*);
    gterm->map = alloc_mem(gterm->map_size);

    gterm->bg_canvas_size = gterm->framebuffer.width * gterm->framebuffer.height * sizeof(uint32_t);
    gterm->bg_canvas = alloc_mem(gterm->bg_canvas_size);

    generate_canvas(gterm);
    gterm_clear(gterm, true);
    gterm_double_buffer_flush(gterm);

    return true;
}

void gterm_deinit(struct gterm_t *gterm)
{
    free_mem(gterm->vga_font_bool, gterm->vga_font_bool_size);
    free_mem(gterm->grid, gterm->grid_size);
    free_mem(gterm->queue, gterm->queue_size);
    free_mem(gterm->map, gterm->map_size);
    free_mem(gterm->bg_canvas, gterm->bg_canvas_size);
}

uint64_t gterm_context_size(struct gterm_t *gterm)
{
    uint64_t ret = 0;

    ret += sizeof(struct gterm_context);
    ret += gterm->grid_size;

    return ret;
}

void gterm_context_save(struct gterm_t *gterm, uint64_t ptr)
{
    memcpy((void*)ptr, &gterm->context, sizeof(struct gterm_context));
    ptr += sizeof(struct gterm_context);

    memcpy((void*)ptr, gterm->grid, gterm->grid_size);
}

void gterm_context_restore(struct gterm_t *gterm, uint64_t ptr)
{
    memcpy(&gterm->context, (void*)ptr, sizeof(struct gterm_context));
    ptr += sizeof(struct gterm_context);

    memcpy(gterm->grid, (void*)ptr, gterm->grid_size);

    for (size_t i = 0; i < (size_t)gterm->rows * gterm->cols; i++)
    {
        size_t x = i % gterm->cols;
        size_t y = i / gterm->cols;

        plot_char(gterm, &gterm->grid[i], x, y);
    }

    if (gterm->context.cursor_status) draw_cursor(gterm);
}

void gterm_full_refresh(struct gterm_t *gterm)
{
    generate_canvas(gterm);

    for (size_t i = 0; i < gterm->rows * gterm->cols; i++)
    {
        size_t x = i % gterm->cols;
        size_t y = i / gterm->cols;

        plot_char(gterm, &gterm->grid[i], x, y);
    }

    if (gterm->context.cursor_status) draw_cursor(gterm);
}
