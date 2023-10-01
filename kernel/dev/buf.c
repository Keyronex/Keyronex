
#include "buf.h"
#include "kdk/kmem.h"

static int buf_cmp(buf_t *a, buf_t *b);

RB_PROTOTYPE_STATIC(buftree, buf, rb_link, buf_cmp);
RB_GENERATE_STATIC(buftree, buf, rb_link, buf_cmp);

/* Compare two buffers' block number - for use by the RB tree code. */
static int
buf_cmp(buf_t *a, buf_t *b)
{
	return a->blkno - b->blkno;
}

void
bufhead_init(bufhead_t *head, DKDevice *device, size_t block_size)
{
	RB_INIT(&head->tree);
	head->device = device;
	head->block_size = block_size;
}

buf_t *
bread(bufhead_t *head, io_blkoff_t block, size_t size)
{
	buf_t *buf, key;

	kassert (size < PGSIZE);

	key.blkno = block;
	if ((buf = RB_FIND(buftree, &head->tree, &key)) != NULL) {
		ke_wait(&buf->mutex, "bread", false, false, -1);
		return buf;
	}

	/*! create and read in */
	buf = kmem_alloc(sizeof(buf_t));
	buf->blkno = block;
	buf->head = head;
	buf->size = size == 0? head->block_size : size;
	ke_mutex_init(&buf->mutex);
	ke_wait(&buf->mutex, "bread", false, false, -1);
	buf->data = kmem_alloc(buf->size);
	RB_INSERT(buftree, &head->tree, buf);

	/*! now read */
	vm_mdl_t *mdl = vm_mdl_create(buf->data, buf->size);
	iop_t *iop = iop_new_read(head->device, mdl, buf->size,
	    block * head->block_size);
	iop_send_sync(iop);
	iop_free(iop);

	return buf;
}

void
buf_release(buf_t *buf)
{
	if (buf == NULL)
		return;
	ke_mutex_release(&buf->mutex);
}
