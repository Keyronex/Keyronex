#ifndef KRX_9P_9PFS_H
#define KRX_9P_9PFS_H

#include "ddk/DKDevice.h"

@class VirtIO9pPort;

@interface NinepFS : DKDevice
+ (BOOL)probeWithProvider: (VirtIO9pPort*) provider;
@end

#endif /* KRX_9P_9PFS_H */
