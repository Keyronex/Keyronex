#ifndef KRX_NET_KEYSOCK_DEV_H
#define KRX_NET_KEYSOCK_DEV_H

#include "ddk/DKDevice.h"

@interface KeySock : DKDevice

+ (BOOL) probeWithProvider: (DKDevice *)provider;

@end

extern void *keysock_dev;

#endif /* KRX_NET_KEYSOCK_DEV_H */
