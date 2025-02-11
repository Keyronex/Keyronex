/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Thu Sep 14 2025.
 */

#ifndef KRX_DEV_SIMPLEFB_H
#define KRX_DEV_SIMPLEFB_H

#include <ddk/DKDevice.h>
#include <ddk/DKFramebuffer.h>

@interface SimpleFB : DKFramebuffer {
}

- (instancetype)initWithAddress:(paddr_t)addr
			  width:(int)width
			 height:(int)height
			  pitch:(int)pitch;

@end

#endif /* KRX_DEV_SIMPLEFB_H */
