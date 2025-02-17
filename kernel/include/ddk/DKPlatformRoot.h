/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Feb 01 2025.
 */

#ifndef KRX_DDK_DKPLATFORMROOT_H
#define KRX_DDK_DKPLATFORMROOT_H

#include <ddk/DKDevice.h>
#include <ddk/DKInterrupt.h>

struct intr_entry;
@class DKPCIBridge;

@protocol DKPlatformRoot

- (void)routePCIPin:(uint8_t)pin
	  forBridge:(DKPCIBridge *)bridge
	       slot:(uint8_t)slot
	   function:(uint8_t)fun
	       into:(out dk_interrupt_source_t *)source;

- (int)allocateLeastLoadedMSIxInterruptForEntry:(struct intr_entry *)entry
				    msixAddress:(out uint32_t *)msixAddress
				       msixData:(out uint32_t *)msixData;

- (int)allocateLeastLoadedMSIInterruptForEntries:(struct intr_entry *)entries
					   count:(size_t)count
				      msiAddress:(out uint32_t *)msiAddress
					 msiData:(out uint32_t *)msiData;

- (DKPlatformInterruptController *)platformInterruptController;

@end

extern DKDevice<DKPlatformRoot> *gPlatformRoot;

#endif /* KRX_DDK_DKPLATFORMROOT_H */
