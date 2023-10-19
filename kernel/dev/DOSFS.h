#ifndef KRX_DEV_DOSFS_H
#define KRX_DEV_DOSFS_H

#include "ddk/DKDevice.h"
#include "kdk/dev.h"
#include "kdk/vfs.h"

@interface DOSFS : DKDevice {
	struct dosfs_state *m_state;
}

+ (BOOL)probeWithVolume:(DKDevice *)volume
	      blockSize:(size_t)blockSize
	     blockCount:(io_blksize_t)blockCount;

-(iop_return_t) dispatchIOP:(iop_t *)iop;

@end

#endif /* KRX_DEV_DOSFS_H */
