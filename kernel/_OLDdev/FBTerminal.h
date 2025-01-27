#ifndef KRX_DEV_FBTERMINAL_H
#define KRX_DEV_FBTERMINAL_H

#include "ddk/DKDevice.h"
#include "ddk/DKFramebuffer.h"
#include "kdk/vm.h"

@interface FBTerminal : DKDevice {
	@public
	struct term_context *term;
}

+ (BOOL)probeWithFramebuffer:(DKFramebuffer *)framebuffer;
@end

void fbterminal_putc(FBTerminal *term, int ch);

extern FBTerminal *system_terminal;

#endif /* KRX_DEV_FBTERMINAL_H */
