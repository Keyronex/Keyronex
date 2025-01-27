
#include <netinet/in.h>

#include <abi-bits/socket.h>
#include <string.h>

#include "9pSockTransport.h"
#include "dev/safe_endian.h"
#include "fs/9p/9p_buf.h"
#include "fs/9p/9pfs.h"
#include "kdk/dev.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "net/keysock_dev.h"

#define PROVIDER ((DKDevice *)m_provider)

static int counter = 0;
static kmutex_t sock_mutex = KMUTEX_INITIALIZER(sock_mutex);

@implementation Socket9pPort

- (instancetype)initWithConnectedSocket:(struct socknode *)socket;
{
	self = [super initWithProvider:keysock_dev];

	kmem_asprintf(obj_name_ptr(self), "socket-9p-%u", counter++);
	m_socket = socket;

	[self registerDevice];
	DKLogAttach(self);

	return self;
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop);
	size_t hdr_size;

	kassert(frame->function == kIOPType9p);

	ke_wait(&sock_mutex, "9p sock serialisation", 0, 0, -1);

	hdr_size = from_leu32(frame->ninep.ninep_in->data->size);
	if (!frame->has_kbuf && frame->mdl != NULL && !frame->mdl->write)
		frame->ninep.ninep_in->data->size = to_leu32(
		    from_leu32(frame->ninep.ninep_in->data->size) +
		    frame->mdl->nentries * PGSIZE);

	iop_t *myop = iop_new_write(keysock_dev,
	    (void *)frame->ninep.ninep_in->data, hdr_size, 0x0);
	myop->stack[0].vnode = (void *)m_socket;
	myop->stack[0].has_kbuf = 1;
	iop_send_sync(myop);
	iop_free(myop);

	if (frame->mdl != NULL && !frame->mdl->write) {
		myop = iop_new_write(keysock_dev, frame->mdl,
		    frame->mdl->nentries * PGSIZE, 0x0);
		myop->stack[0].vnode = (void *)m_socket;
		myop->stack[0].has_kbuf = 0;
		iop_send_sync(myop);
		iop_free(myop);
	}

	myop = iop_new_read(keysock_dev, (void *)frame->ninep.ninep_out->data,
	    sizeof(struct ninep_hdr), 0x0);
	myop->stack[0].vnode = (void *)m_socket;
	myop->stack[0].has_kbuf = 1;
	iop_send_sync(myop);
	iop_free(myop);

	int32_t bytes_left = from_leu32(frame->ninep.ninep_out->data->size) -
	    sizeof(struct ninep_hdr);
	size_t hdr_ext = frame->ninep.ninep_out->bufsize -
	    sizeof(struct ninep_hdr);

	kassert(bytes_left >= 0);
	if (bytes_left > 0) {
		size_t hdr_left = MIN2(bytes_left, hdr_ext);
		myop = iop_new_read(keysock_dev,
		    ((void *)frame->ninep.ninep_out->data) +
			sizeof(struct ninep_hdr),
		    hdr_left, 0x0);
		myop->stack[0].vnode = (void *)m_socket;
		myop->stack[0].has_kbuf = 1;
		iop_send_sync(myop);
		iop_free(myop);
	}

	bytes_left -= hdr_ext;

	if (frame->mdl == NULL && bytes_left > 0)
		kfatal("unexpected\n");

	if (frame->mdl && frame->mdl->write) {
		myop = iop_new_read(keysock_dev, frame->mdl, bytes_left, 0x0);
		myop->stack[0].vnode = (void *)m_socket;
		myop->stack[0].has_kbuf = 0;
		iop_send_sync(myop);
		iop_free(myop);
	} else if (bytes_left > 0) {
		kfatal("Disaster\n");
	}

	ke_mutex_release(&sock_mutex);

	return kIOPRetCompleted;
}

@end
