#ifndef KRX_DEV_DOSFS_H
#define KRX_DEV_DOSFS_H

#include "ddk/DKDevice.h"
#include "kdk/dev.h"

@interface DOSFS : DKDevice {
	struct dosfs_state *state;
}

+ (BOOL)probeWithVolume:(DKDevice *)volume
	      blockSize:(size_t)blockSize
	     blockCount:(io_blksize_t)blockCount;

@end

#endif /* KRX_DEV_DOSFS_H */
