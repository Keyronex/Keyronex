/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Feb 01 2025.
 */

#ifndef KRX_DDK_DKPLATFORMROOT_H
#define KRX_DDK_DKPLATFORMROOT_H

#include <ddk/DKDevice.h>
#include <ddk/DKInterrupt.h>

struct intr_entry;

@protocol DKPlatformRoot

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
