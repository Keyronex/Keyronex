#ifndef KRX_DEV_EXT2FS_H
#define KRX_DEV_EXT2FS_H

#include "ddk/DKDevice.h"
#include "kdk/dev.h"

struct ext2_super_block;

@interface Ext2FS : DKDevice {
	struct ext2fs_state *state;
}

+ (BOOL)probeWithVolume:(DKDevice *)volume
	      blockSize:(size_t)blockSize
	     blockCount:(io_blksize_t)blockCount;
- (instancetype)initWithVolume:(DKDevice *)volume
		     blockSize:(size_t)blockSize
		    blockCount:(io_blksize_t)blockCount
		    superBlock:(struct ext2_super_block *)sb;

@end

#endif /* KRX_DEV_EXT2FS_H */
