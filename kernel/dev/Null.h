#ifndef KRX_DEV_NULL_H
#define KRX_DEV_NULL_H

#include "ddk/DKDevice.h"

@interface Null : DKDevice

+ (instancetype)null;
+ (BOOL)probeWithProvider:(DKDevice *)provider;

@end

#endif /* KRX_DEV_NULL_H */
