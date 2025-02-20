#ifndef KRX_SOCK_SOCK9PTRANSPORT_H
#define KRX_SOCK_SOCK9PTRANSPORT_H

#include "ddk/DKDevice.h"
#include "dev/safe_endian.h"

@interface Socket9pPort : DKDevice {
    @public
	struct socknode *m_socket;
}

- (instancetype)initWithConnectedSocket:(struct socknode *)socket;

@end

#endif /* KRX_SOCK_SOCK9PTRANSPORT_H */
