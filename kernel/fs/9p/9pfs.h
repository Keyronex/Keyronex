#ifndef KRX_9P_9PFS_H
#define KRX_9P_9PFS_H

#include "ddk/DKDevice.h"

@class VirtIO9pPort;

@interface NinepFS : DKDevice {
	struct ninepfs_state *m_state;
}

@end

#endif /* KRX_9P_9PFS_H */
