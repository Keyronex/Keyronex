#ifndef FBTERM_H_
#define FBTERM_H_

#include <posix/tty.h>

#ifdef __OBJC__
#include <dev/LimineFB.h>
#include <devicekit/DKDevice.h>

#include "./term.h"

@interface FBConsole : DKDevice {
	LimineFB *_fb;
	tty_t tty;
	struct framebuffer_t frm;
	struct term_t	     term;
	struct style_t	     style;
	struct image_t	     img;
	struct background_t  back;
	struct font_t	     font;
}

+ (BOOL)probeWithFB:(LimineFB *)fb;

- initWithFB:(LimineFB *)fb;

- (void)flush;
- (void)putc:(int)c;

#if 0
/* accept a character */
- (void)input:(int)c;
/* accept multiple characters up till NULL byte */
- (void)inputChars:(const char *)cs;
#endif

@end
#else
extern void *syscon;
#endif

extern tty_t * sctty;
void sysconputc(int c);
void sysconflush(void);

#endif /* FBTERM_H_ */
