/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#ifndef KRX_FBCONS_FBCONS_HH
#define KRX_FBCONS_FBCONS_HH

#include <linux/fb.h>
#include "../mdf/mdfdev.hh"

class FBConsole : public Device {
    static FBConsole *syscon;
	struct limine_framebuffer *fb;
	struct term_context *term;
    bool inhibited = false;

    static void puts(const char *buf, size_t len);
    static void printstats();
    static void getsize(struct winsize *ws);
    static void getfbinfo(struct fb_var_screeninfo *var, struct fb_fix_screeninfo * fix);
    static void inhibit();

    public:
	FBConsole(Device*provider);
};

#endif /* KRX_FBCONS_FBCONS_HH */
