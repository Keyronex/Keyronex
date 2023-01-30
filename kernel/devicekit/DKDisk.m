/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#include <sys/param.h>

#include <nanokern/thread.h>

#include <errno.h>

#include "DKDisk.h"

#define selfDelegate ((DKDrive<DKDriveMethods> *)self)

static int driveIDCounter = 0;

@implementation DKDrive

@synthesize driveID = m_driveID;
@synthesize blockSize = m_blockSize;
@synthesize nBlocks = m_nBlocks;
@synthesize maxBlockTransfer = m_maxBlockTransfer;

- init
{
	self = [super init];
	if (self) {
		m_driveID = driveIDCounter++;
	}
	return self;
}

struct complete_sync_data {
	kevent_t ev;
	ssize_t	 result;
};

static void
complete_sync(void *data, ssize_t result)
{
	struct complete_sync_data *sync = data;
	sync->result = result;
	nk_event_signal(&sync->ev);
}

#if 0
static void
multiblock_callback(void *data, ssize_t result)
{
	struct dk_diskio_completion *comp = data;
	return [comp->drive continueMultiblock:comp result:result];
}

- (void)continueMultiblock:(struct dk_diskio_completion *)completion
		    result:(ssize_t)result
{
	int    r;
	size_t nbytes;
	size_t offset;

	kassert(result > 0);

	completion->bytes_left -= result;
	kassert(completion->bytes_left >= 0);

	if (completion->bytes_left == 0)
		return completion->final_callback(completion->final_data,
		    completion->bytes_total);

	offset = completion->bytes_total - completion->bytes_left;
	nbytes = MIN(m_maxBlockTransfer * m_blockSize, completion->bytes_left);

	completion->buf->offset += result;

	r = completion->strategy == kDKRead ?
	    [selfDelegate strategy:kDKRead
			   blocks:nbytes / m_blockSize
			       at:completion->initial + offset / m_blockSize
			   buffer:completion->buf
		       completion:completion] :
	    [selfDelegate strategy:kDKWrite
			   blocks:nbytes / m_blockSize
			       at:completion->initial + offset / m_blockSize
			   buffer:completion->buf
		       completion:completion];

	kassert (r == 0);
}
#endif

- (int)commonIO:(dk_strategy_t)strategy
	  bytes:(size_t)nBytes
	     at:(off_t)offset
	 buffer:(vm_mdl_t *)buf
     completion:(struct dk_diskio_completion *)completion
{
	struct dk_diskio_completion comp;
	struct complete_sync_data   sync;
	int			    r;

	if (!completion) {
		completion = &comp;
		comp.callback = complete_sync;
		comp.data = &sync;
		nk_event_init(&sync.ev, false);
	} else
		comp.data = NULL;

	if (offset % m_blockSize != 0 || nBytes % m_blockSize != 0) {
		DKDevLog(self,
		    "(DKDrive) Strategy request with non-block-aligned"
		    "offset or size received - not yet handled.\n");
		return -EOPNOTSUPP;
	}

	if (buf->offset % PGSIZE != 0) {
		DKDevLog(self,
		    "(DKDrive) Strategy request with non-page-aligned"
		    "offset received - not yet handled.\n");
		return -EOPNOTSUPP;
	}

	if (nBytes > m_maxBlockTransfer * m_blockSize) {
		DKDevLog(self, "Excessive request received, let's try it.\n");
		return -ENOTSUP;
#if 0
		completion->drive = selfDelegate;
		completion->bytes_left = nBytes;
		completion->bytes_total = nBytes;
		completion->final_callback = completion->callback;
		completion->final_data = completion->data;
		completion->callback = multiblock_callback;
		completion->data = completion;
		nBytes = m_maxBlockTransfer * m_blockSize;
#endif
	}

	r = strategy == kDKRead ? [selfDelegate strategy:kDKRead
						  blocks:nBytes / m_blockSize
						      at:offset / m_blockSize
						  buffer:buf
					      completion:completion] :
				  [selfDelegate strategy:kDKWrite
						  blocks:nBytes / m_blockSize
						      at:offset / m_blockSize
						  buffer:buf
					      completion:completion];

	if (r != 0)
		return r;

	if (comp.data) {
		/* synchronous case */
		r = nk_wait(&sync.ev, "DKDrive commonIO generic wait", false,
		    false, -1);
		assert(r == kKernWaitStatusOK);
		return sync.result;
	} else
		return 0;
}

- (int)readBytes:(size_t)nBytes
	      at:(off_t)offset
      intoBuffer:(vm_mdl_t *)buf
      completion:(struct dk_diskio_completion *)completion
{
	return [self commonIO:kDKRead
			bytes:nBytes
			   at:offset
		       buffer:buf
		   completion:completion];
}

- (int)writeBytes:(size_t)nBytes
	       at:(off_t)offset
       fromBuffer:(vm_mdl_t *)buf
       completion:(struct dk_diskio_completion *)completion
{
	return [self commonIO:kDKWrite
			bytes:nBytes
			   at:offset
		       buffer:buf
		   completion:completion];
}

@end
