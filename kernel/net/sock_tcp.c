#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>

#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/tcpbase.h"
#include "buf.h"
#include "net.h"
#include "util.h"

struct socknode {
	union {
		struct tcp_pcb *tcpcb;
		struct udp_pcb *udpcb;
		struct raw_pcb *rawpcb;
	};

	kspinlock_t lock;

	STAILQ_HEAD(, sbuf) receive_queue;
	iop_queue_t receive_iops;

	iop_queue_t send_iops;
	iop_t *send_queue_current_iop;
	size_t send_queue_bytes;
	size_t send_queue_issued;
	size_t send_queue_confirmed;

	iop_t *connect_iop;
};

#define LOCK_TCP_SOCK(SO) LOCK_LWIP()
#define UNLOCK_TCP_SOCK(SO, IPL) UNLOCK_LWIP(IPL)
#define LOCK_TCP_SOCK_NOSPL(SO) LOCK_LWIP_NOSPL()
#define UNLOCK_TCP_SOCK_NOSPL(SO) UNLOCK_LWIP_NOSPL()

/*
 * binding, connecting, listening
 */

err_t
connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
	struct socknode *so = (void *)arg;
	iop_t *iop = so->connect_iop;
	iop->result.result = err_to_errno(err);
	iop_continue(iop, kIOPRetCompleted);
	return ERR_OK;
}

static iop_return_t
dispatch_connect(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);
	struct socknode *so = (void *)frame->vnode;
	ip_addr_t ip;
	uint16_t port;
	err_t err;
	ipl_t ipl;

	addr_unpack_ip(frame->connect.sockaddr, &ip, &port);

	ipl = ke_spinlock_acquire(&so->lock);

	if (so->tcpcb->state == SYN_SENT) {
		iop->result.result = EALREADY;
		goto fail;
	} else if (so->tcpcb->state == LISTEN) {
		iop->result.result = EOPNOTSUPP;
		goto fail;
	} else if (so->tcpcb->state != CLOSED) {
		iop->result.result = EISCONN;
		goto fail;
	}

	so->connect_iop = iop;

	LOCK_TCP_SOCK_NOSPL(so->tcpcb);
	err = tcp_connect(so->tcpcb, &ip, port, connected_cb);
	UNLOCK_TCP_SOCK_NOSPL(so->tcpcb);
	if (err != ERR_OK) {
		iop->result.result = err_to_errno(err);
		goto fail;
	}

	ke_spinlock_release(&so->lock, ipl);
	return kIOPRetPending;

fail:
	so->connect_iop = NULL;
	ke_spinlock_release(&so->lock, ipl);
	return kIOPRetCompleted;
}

/*
 * receiving
 */

static err_t
recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	struct socknode *sock = arg;
	sbuf_t *sb;
	io_off_t offset = 0;

	if (p == NULL)
		kfatal("Handle TCP peer disconnect\n");

	kassert(err == ERR_OK);
	kassert(p->tot_len > 0);

	/*
	 * notes:
	 * - iop completion could be done by queuing them on a list
	 *   and completing them in a DPC if we run into problems around locks
	 *   being held here
	 * - count of bytes available in receive buffer, and of bytes available
	 *   in queued iops, could be kept, to allow an early decision to e.g.
	 *   allocate an sbuf or now (good for permitting failure of the whole
	 *   thing.)
	 */

	while (!TAILQ_EMPTY(&sock->receive_iops) && offset != p->tot_len) {
		iop_t *iop = TAILQ_FIRST(&sock->receive_iops);
		iop_frame_t *frame;
		size_t to_write;

		frame = iop_stack_current(iop);

		to_write = MIN2(frame->rw.bytes - frame->rw.offset,
		    p->tot_len - offset);

		if (frame->has_kbuf) {
			pbuf_copy_partial(p, frame->kbuf + frame->rw.offset,
			    to_write, offset);
		}
		else
			pbuf_copy_partial_into_mdl(p, frame->mdl,
			    frame->rw.offset, to_write, offset);

		frame->rw.offset += to_write;
		offset += to_write;

		if (frame->rw.offset == frame->rw.bytes) {
			TAILQ_REMOVE(&sock->receive_iops, iop, dev_queue_entry);
			iop_continue(iop, kIOPRetCompleted);
		} else
			break;
	}

	tcp_recved(sock->tcpcb, offset);

	if (offset != p->tot_len) {
		sb = sb_alloc(p->tot_len - offset);
		if (sb == NULL)
			return ERR_MEM;

		pbuf_copy_partial(p, sb->data, p->tot_len - offset, offset);

		STAILQ_INSERT_TAIL(&sock->receive_queue, sb, queue_entry);
	}

	pbuf_free(p);

	return ERR_OK;
}

static iop_return_t
dispatch_read(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);
	struct socknode *so = (void *)frame->vnode;
	size_t nreceived = 0;
	int r = kIOPRetPending;
	ipl_t ipl;

	ipl = LOCK_TCP_SOCK(so->tcpcb);
	while (!STAILQ_EMPTY(&so->receive_queue)) {
		sbuf_t *sbuf = STAILQ_FIRST(&so->receive_queue);
		size_t to_copy = MIN2(frame->rw.bytes - frame->rw.offset, sbuf->len - sbuf->offset);

		if (frame->has_kbuf) {
			memcpy(frame->kbuf + frame->rw.offset,
			    sbuf->data + sbuf->offset, to_copy);
		}
		else
			vm_mdl_copy_into(frame->mdl, frame->rw.offset,
			    sbuf->data + sbuf->offset, to_copy);

		nreceived += to_copy;
		sbuf->offset += to_copy;
		frame->rw.offset += to_copy;
		if (sbuf->offset == sbuf->len) {
			STAILQ_REMOVE_HEAD(&so->receive_queue, queue_entry);
		}

		if (frame->rw.offset == frame->rw.bytes) {
			r = kIOPRetCompleted;
			break;
		}
	}

	if (r != kIOPRetCompleted)
		TAILQ_INSERT_TAIL(&so->receive_iops, iop, dev_queue_entry);

	tcp_recved(so->tcpcb, nreceived);

	UNLOCK_TCP_SOCK(so->tcpcb, ipl);

	return r;

}

/*
 * sending
 */

static size_t
frame_contig_bytes(iop_frame_t *frame)
{
	if (frame->has_kbuf)
		return frame->rw.bytes - frame->rw.offset;
	else
		return vm_mdl_contig_bytes(frame->mdl, frame->rw.offset);
}

static void *
frame_addr(iop_frame_t *frame)
{
	if (frame->has_kbuf)
		return frame->kbuf + frame->rw.offset;
	else
		return (void *)P2V(vm_mdl_paddr(frame->mdl, frame->rw.offset));
}

static void
try_send_more(struct socknode *sock)
{
	while (sock->send_queue_issued != sock->send_queue_bytes && sock->send_queue_current_iop != NULL) {
		size_t sndbuf, to_send;
		iop_t *iop = sock->send_queue_current_iop;
		iop_frame_t *frame = iop_stack_current(iop);
		err_t err;

		sndbuf = tcp_sndbuf(sock->tcpcb);
		if (sndbuf == 0)
			break;

		if (frame_contig_bytes(frame) > PGSIZE)
			kfatal("Fucked up error\n");
		to_send = MIN2(frame_contig_bytes(frame), sndbuf);

		err = tcp_write(sock->tcpcb, frame_addr(frame), to_send,
		    (sock->send_queue_issued + to_send ==
			sock->send_queue_bytes) ?
			0 :
			TCP_WRITE_FLAG_MORE);
		kassert(err == 0);

		sock->send_queue_issued += to_send;
		frame->rw.offset += to_send;
		kassert(frame->rw.offset <= frame->rw.bytes);
		if (frame->rw.offset == frame->rw.bytes)
			sock->send_queue_current_iop = TAILQ_NEXT(iop,
			    dev_queue_entry);
	}
}

static err_t
sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len)
{
	struct socknode *so = (void *)arg;

	while (len) {
		iop_t *iop = TAILQ_FIRST(&so->send_iops);
		iop_frame_t *frame = iop_stack_current(iop);
		size_t sent = MIN2(frame->rw.bytes - so->send_queue_confirmed,
		    len);

		kassert(sent <= UINT16_MAX);

		so->send_queue_confirmed += sent;
		if (so->send_queue_confirmed == frame->rw.bytes) {
			TAILQ_REMOVE(&so->send_iops, iop, dev_queue_entry);
			so->send_queue_confirmed -= frame->rw.bytes;
			so->send_queue_issued -= frame->rw.bytes;
			iop_continue(iop, kIOPRetCompleted);
		}

		len -= sent;
	}

	try_send_more(so);

	return ERR_OK;
}

static iop_return_t
dispatch_write(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);
	struct socknode *so = (void *)frame->vnode;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&so->lock);

	if (so->tcpcb->state != ESTABLISHED)
		kfatal("unconnected socket\n");

	LOCK_TCP_SOCK_NOSPL(so->tcpcb);
	TAILQ_INSERT_TAIL(&so->send_iops, iop, dev_queue_entry);
	so->send_queue_bytes += frame->rw.bytes;
	if (so->send_queue_current_iop == NULL)
		so->send_queue_current_iop = iop;
	try_send_more(so);
	UNLOCK_TCP_SOCK_NOSPL(so->tcpcb);

	ke_spinlock_release(&so->lock, ipl);

	return kIOPRetPending;
}

/*
 * IOP dispatch & all other stuff
 */

iop_return_t
keysock_dispatch_iop(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);

	switch (frame->function) {
	case kIOPTypeConnect:
		return dispatch_connect(iop);

	case kIOPTypeRead:
		return dispatch_read(iop);

	case kIOPTypeWrite:
		return dispatch_write(iop);

	default:
		kfatal("Unhandled\n");
	}
}

struct socknode *
new_tcpnode(void)
{
	struct socknode *so;

	so = kmem_alloc(sizeof(*so));

	so->tcpcb = tcp_new();
	tcp_arg(so->tcpcb, so);
	kassert(so->tcpcb != NULL);

	ke_spinlock_init(&so->lock);
	STAILQ_INIT(&so->receive_queue);
	TAILQ_INIT(&so->receive_iops);

	TAILQ_INIT(&so->send_iops);
	so->send_queue_bytes = 0;
	so->send_queue_confirmed = 0;
	so->send_queue_issued = 0;
	so->send_queue_current_iop = NULL;

	so->connect_iop = NULL;

	tcp_recv(so->tcpcb, recv_cb);
	tcp_sent(so->tcpcb, sent_cb);

	return so;
}
