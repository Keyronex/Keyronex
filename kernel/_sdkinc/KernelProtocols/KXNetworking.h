#ifndef KRX_KERNELPROTOCOLS_KXNETWORK_H
#define KRX_KERNELPROTOCOLS_KXNETWORK_H

#import <ObjFW/OFArray.h>

@protocol KXNetworking

/* array of OFData containing OFSocketAddress's */
- (OFArray OF_GENERIC(OFData *) *)getAddressesOfInterface:(const char *)ifName;

- (int)setAddressOfInterface:(const char *)ifName
		   toAddress:(struct sockaddr *)addr
		     netmask:(struct sockaddr *)netmask
		     gateway:(struct sockaddr *)gateway;

@end

#endif /* KRX_KERNELPROTOCOLS_KXNETWORK_H */
