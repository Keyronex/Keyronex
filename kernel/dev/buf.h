#ifndef KRX_DEV_BUF_H
#define KRX_DEV_BUF_H

#include "kdk/dev.h"
#include "kdk/nanokern.h"

/*!
 * A block buffer.
 */
typedef struct buf {
	/*! Lock on contents. */
	kmutex_t mutex;
	/*! Owner. */
	struct bufhead *head;
	/*! Buffer tree linkage. */
	RB_ENTRY(buf) rb_link;
	/*! Whether the buf is busy. If so, it can't be put back to disk. */
	bool busy;
	/*! Size fo the block. */
	uint16_t size;
	/*! FS logical block number that the buf represents. */
	io_blkoff_t blkno;
	/*! Pointer to the actual data. */
	char *data;
} buf_t;

typedef RB_HEAD(buftree, buf) buftree_t;

typedef struct bufhead {
	void *device;
	size_t block_size;
	buftree_t tree;
} bufhead_t;

void bufhead_init(bufhead_t *head, DKDevice *device, size_t block_size);
buf_t *bread(bufhead_t *head, io_blkoff_t block, size_t size);
void bwrite(buf_t *buf);
void buf_release(buf_t *buf);

#endif /* KRX_DEV_BUF_H */
