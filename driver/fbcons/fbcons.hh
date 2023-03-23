/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#ifndef KRX_FBCONS_FBCONS_HH
#define KRX_FBCONS_FBCONS_HH

#include "../mdf/mdfdev.hh"

class FBConsole : public Device {
    static FBConsole *syscon;
	struct limine_framebuffer *fb;
	struct term_context *term;

    static void puts(const char *buf, size_t len);
    static void printstats();

    public:
	FBConsole(Device*provider);
};

#endif /* KRX_FBCONS_FBCONS_HH */
