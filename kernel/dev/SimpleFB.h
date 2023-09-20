#ifndef KRX_DEV_LIMINEFB_H
#define KRX_DEV_LIMINEFB_H

#include "ddk/DKDevice.h"
#include "ddk/DKFramebuffer.h"

@interface SimpleFB : DKFramebuffer {
}

+ (BOOL)probeWithProvider:(DKDevice *)provider
		  address:(paddr_t)addr
		    width:(int)width
		   height:(int)height
		    pitch:(int)pitch;

@end

#endif /* KRX_DEV_LIMINEFB_H */
