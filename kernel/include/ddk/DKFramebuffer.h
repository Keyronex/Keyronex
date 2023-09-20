#ifndef KRX_DDK_DKFRAMEBUFFER_H
#define KRX_DDK_DKFRAMEBUFFER_H

#include "ddk/DKDevice.h"
#include "kdk/vm.h"

struct dk_framebuffer_info {
	/*! framebuffer address */
	paddr_t address;
	/*! dimensions*/
	int width, height;
	/* bits per pixel */
	int pitch;
};

/*! abstract class of framebuffers */
@interface DKFramebuffer : DKDevice {
	struct dk_framebuffer_info m_info;
}

@property struct dk_framebuffer_info info;

@end

#endif /* KRX_DDK_DKFRAMEBUFFER_H */
