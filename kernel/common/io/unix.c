/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Wed Feb 25 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file unix.c
 * @brief Unix (loopback) transport provider. For Unix-domain.
 *
 * The `rq` field, and therefore the existence of a stream associated with an
 * endpoint, is assured by holding that endpoint's lock.
 *
 * This allows cross-endpoint enqueuing when that lock is held, because
 * str_ingress_enqueue doesn't need to acquire the other stream's lock.
 *
 * Since all put/server routines within a stream are serialised by that stream's
 * mutex, we do actually have an assurance that while we're executing, no one
 * but ourselves can possibly put anything to our peer stream. (This will be
 * useful for flow control later. We might imagine that an endpoint lock
 * protects floco state. But this is for exploring later when we look into flow
 * control.)
 *
 * We preallocate control messages like t_discon_ind at the point of connect
 * request/response so we can put those up.
 *
 * The bindings RB tree is vestigial and not actually used anymore, because
 * higher-level layers provide existence guarantees on ACCEPTOR_id and sux_rq.
 * It can probably go away and I don't see it being used for anything else.
 */

#include <sys/errno.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>
#include <sys/tree.h>

#include <stdatomic.h>

enum ux_state {
	UX_UNBOUND,
	UX_BOUND,
	UX_LISTENING,
	UX_CONNECTING,
	UX_CONNECTED,
	UX_DISCONNECTED,
};

typedef struct ux_endp {
	kmutex_t lock;
	atomic_uint refcnt;
	queue_t *rq;
	enum ux_state state;
	bool closing;
	struct ux_endp *peer;

	RB_ENTRY(ux_endp) rb_entry;

	/* UX_CONNECTING */
	LIST_ENTRY(ux_endp) conninds_entry;
	uint32_t my_seqno;
	struct ux_endp *connecting_listener;

	/* UX_LISTENER */
	LIST_HEAD(, ux_endp) conninds;
	uint32_t next_seqno;

	/* UX_CONNECTING/UX_CONNECTED */
	mblk_t *discon_ind_mp;
} ux_endp_t;

RB_HEAD(ux_endp_tree, ux_endp);

static int ep_cmp(ux_endp_t *a, ux_endp_t *b);

static int ux_ropen(queue_t *rq, void *);
static void ux_rclose(queue_t *rq);
static void ux_rput(queue_t *rq, mblk_t *);
static void ux_wput(queue_t *wq, mblk_t *);

RB_GENERATE_STATIC(ux_endp_tree, ux_endp, rb_entry, ep_cmp);

static krwlock_t ux_bindings_rwlock = KRWLOCK_INITIALISER;
static struct ux_endp_tree ux_bindings = RB_INITIALIZER(&ux_bindings);
static struct qinit ux_rinit = {
	.qopen = ux_ropen,
	.qclose = ux_rclose,
	.putp = ux_rput,
};
static struct qinit ux_winit = {
	.putp = ux_wput,
};

struct streamtab ux_stream_streamtab = {
	.rinit = &ux_rinit,
	.winit = &ux_winit,
};

struct streamtab ux_dgram_streamtab = {
	.rinit = &ux_rinit,
	.winit = &ux_winit,
};

static int
ep_cmp(ux_endp_t *a, ux_endp_t *b)
{
	if (a->rq < b->rq)
		return -1;
	else if (a->rq > b->rq)
		return 1;
	else
		return 0;
}

static void
ep_retain(ux_endp_t *ep)
{
	atomic_fetch_add_explicit(&ep->refcnt, 1, memory_order_relaxed);
}

static void
ep_release(ux_endp_t *ep)
{
	if (atomic_fetch_sub_explicit(&ep->refcnt, 1, memory_order_acq_rel) ==
	    1)
		kmem_free(ep, sizeof(*ep));
}

/* e.g. for me, peer */
static void
ux_lock_two_eps(ux_endp_t *x, ux_endp_t *y, const char *why)
{
	if (x < y) {
		ke_mutex_enter(&x->lock, why);
		ke_mutex_enter(&y->lock, why);
	} else if (x > y) {
		ke_mutex_enter(&y->lock, why);
		ke_mutex_enter(&x->lock, why);
	} else {
		kfatal("ux_lock_two_eps: identical endpoints");
	}
}

static inline void
swap_eps(ux_endp_t **a, ux_endp_t **b)
{
	ux_endp_t *t = *a;
	*a = *b;
	*b = t;
}

/* e.g. for me, peer, acceptor */
static void
ux_lock_three_eps(ux_endp_t *x, ux_endp_t *y, ux_endp_t *z, const char *why)
{
	ux_endp_t *e[3] = { x, y, z };

	if (e[0] > e[1])
		swap_eps(&e[0], &e[1]);
	if (e[1] > e[2])
		swap_eps(&e[1], &e[2]);
	if (e[0] > e[1])
		swap_eps(&e[0], &e[1]);

	if (e[0] == e[1] || e[1] == e[2])
		kfatal("ux_lock_three_eps: identical endpoints");

	ke_mutex_enter(&e[0]->lock, why);
	ke_mutex_enter(&e[1]->lock, why);
	ke_mutex_enter(&e[2]->lock, why);
}

static int
ux_ropen(queue_t *rq, void *)
{
	ux_endp_t *ep;

	ep = kmem_alloc(sizeof(*ep));
	if (ep == NULL)
		return -ENOMEM;

	ke_mutex_init(&ep->lock);
	atomic_store_explicit(&ep->refcnt, 1, memory_order_relaxed);
	ep->rq = rq;
	ep->peer = NULL;
	ep->state = UX_UNBOUND;
	ep->closing = false;

	ep->my_seqno = -1;

	LIST_INIT(&ep->conninds);
	ep->next_seqno = 1;

	ke_rwlock_enter_write(&ux_bindings_rwlock, "ux_ropen");
	RB_INSERT(ux_endp_tree, &ux_bindings, ep);
	ke_rwlock_exit_write(&ux_bindings_rwlock);

	rq->ptr = rq->other->ptr = ep;

	return 0;
}

static void
ux_send_discon_ind(ux_endp_t *ep)
{
	mblk_t *mp = ep->discon_ind_mp;
	struct T_discon_ind *di;

	kassert(ke_mutex_held(&ep->lock));
	kassert(mp != NULL);

	mp->db->type = M_PROTO;
	di = (typeof(di))mp->rptr;
	di->PRIM_type = T_DISCON_IND;
	di->DISCON_reason = ECONNRESET;
	di->SEQ_number = -1;
	mp->wptr = mp->rptr + sizeof(*di);

	ep->discon_ind_mp = NULL;
	str_ingress_putq(ep->rq->stdata, mp);
}

static void
ux_rclose(queue_t *rq)
{
	ux_endp_t *ep = rq->ptr;
	ux_endp_t *peer = NULL;
	ux_endp_t *listener = NULL;
	enum ux_state state;

	ke_rwlock_enter_write(&ux_bindings_rwlock, "ux_rclose rb");
	RB_REMOVE(ux_endp_tree, &ux_bindings, ep);
	ke_rwlock_exit_write(&ux_bindings_rwlock);

	ke_mutex_enter(&ep->lock, "ux_rclose quiesce");

	ep->closing = true; /* denies new connections */

	state = ep->state;

	/*
	 * First take references to peer/listener while our lock is held;
	 * We can't do much with them without later reacquiring the locks in the
	 * defined order.
	 * It should be impossible for them to change to anything but NULL
	 * henceforth.
	 */

	peer = ep->peer;
	if (peer != NULL)
		ep_retain(peer);

	listener = ep->connecting_listener;
	if (listener != NULL)
		ep_retain(listener);

	ke_mutex_exit(&ep->lock);

	/* Second, if connected, disconnect peer. */
do_peer:
	if (peer != NULL) {
		ux_lock_two_eps(ep, peer, "ux_rclose connected");

		if (ep->peer == peer) {
			kassert(peer->peer == ep);
			kassert(ep->state == UX_CONNECTED);
			kassert(peer->state == UX_CONNECTED);

			ep_release(peer); /* ep's ref on peer */
			ep->peer = NULL;

			ep_release(ep); /* peer's ref on ep */
			peer->peer = NULL;

			peer->state = UX_DISCONNECTED;
			ux_send_discon_ind(peer);
		} else {
			/* they disconnected first */
		}

		ke_mutex_exit(&ep->lock);
		ke_mutex_exit(&peer->lock);

		ep_release(peer); /* local ref from first step */
		peer = NULL;
	}

	/* Third, if connecting. detach from listener's pending list. */
	if (listener != NULL) {
		ux_lock_two_eps(ep, listener, "ux_rclose connecting");

		if (ep->state == UX_CONNECTED) {
			/* connection completed while locks released? */
			peer = ep->peer;
			ep_retain(peer);
			ke_mutex_exit(&ep->lock);
			ke_mutex_exit(&listener->lock);
			ep_release(listener);
			listener = NULL;
			goto do_peer;
		} else if (ep->state == UX_CONNECTING) {
			ux_endp_t *var;

			kassert(ep->connecting_listener == listener);

			/* paranoid check: are we in listener's conninds? */
			LIST_FOREACH(var, &listener->conninds, conninds_entry)
				if (var == ep)
					break;
			kassert(var == ep);

			LIST_REMOVE(ep, conninds_entry);
			ep_release(ep);	 /* listener connind's ref on ep */

			ep->connecting_listener = NULL;
			ep_release(listener); /* ep's ref on listener */
		}

		ke_mutex_exit(&ep->lock);
		ke_mutex_exit(&listener->lock);

		ep_release(listener); /* local ref from first step */
		listener = NULL;
	}

	/* Fourth, if we were listening, drain pending conninds. */
	if (state == UX_LISTENING) {
		for (;;) {
			ux_endp_t *c;

			ke_mutex_enter(&ep->lock, "ux_rclose drain");
			c = LIST_FIRST(&ep->conninds);
			if (c != NULL)
				ep_retain(c); /* local ref */
			ke_mutex_exit(&ep->lock);

			if (c == NULL)
				break;

			ux_lock_two_eps(ep, c, "ux_rclose abort connind");

			/*
			 * Paranoid check: it shouldn't have been able to
			 * complete connecting when we are being closed. if
			 * we're being closed then no one could have accepted!
			 */
			kassert(!(c->state == UX_CONNECTED && c->peer == ep));

			if (c->state == UX_CONNECTING &&
			    c->connecting_listener == ep) {
				LIST_REMOVE(c, conninds_entry);
				ep_release(c);	/* conninds ref on connector */

				c->connecting_listener = NULL;
				ep_release(ep); /* connector's ref on ep */

				c->state = UX_DISCONNECTED;
				ux_send_discon_ind(c);

			} else {
				ux_endp_t *var;

				/*
				 * Paranoid check: shouldn't then still be on
				 * our conninds!
				 */
				LIST_FOREACH(var, &ep->conninds, conninds_entry)
					if (var == c)
						break;
				kassert(var == NULL);
			}

			ke_mutex_exit(&ep->lock);
			ke_mutex_exit(&c->lock);

			ep_release(c); /* local ref */
		}
	}

	ke_mutex_enter(&ep->lock, "ux_rclose finally");
	if (ep->discon_ind_mp != NULL)
		str_freeb(ep->discon_ind_mp);

	ep->state = UX_DISCONNECTED;
	ep->rq = NULL;
	ke_mutex_exit(&ep->lock);

	/* Finally, drop the ref owned by the stream head that's closing. */
	ep_release(ep);
}

static void
ux_rput(queue_t *rq, mblk_t *mp)
{
	ktodo();
}

static void
ux_reply_ok_ack(queue_t *wq, mblk_t *mp, enum T_prim correct_prim)
{
	struct T_ok_ack *oa = (typeof(oa))mp->rptr;

	kassert(mp->wptr - mp->rptr >= sizeof(*oa));

	oa->PRIM_type = T_OK_ACK;
	oa->CORRECT_prim = correct_prim;
	mp->wptr = mp->rptr + sizeof(*oa);

	str_qreply(wq, mp);
}

static void
ux_reply_error_ack(queue_t *wq, mblk_t *mp, enum T_prim error_prim,
    int unix_error)
{
	struct T_error_ack *ea = (typeof(ea))mp->rptr;

	kassert(mp->wptr - mp->rptr >= sizeof(*ea));

	ea->PRIM_type = T_ERROR_ACK;
	ea->ERROR_prim = error_prim;
	ea->UNIX_error = unix_error;
	mp->wptr = mp->rptr + sizeof(*ea);

	str_qreply(wq, mp);
}

static void
ux_wput_bind_req(queue_t *wq, mblk_t *mp)
{
	ux_endp_t *ep = wq->ptr;
	struct T_bind_req *br = (typeof(br))mp->rptr;

	ke_mutex_enter(&ep->lock, "ux_wput_bind_req");

	if (!(ep->state == UX_UNBOUND) &&
	    !(ep->state == UX_BOUND &&
	    /* current connind == 0 && */ br->CONIND_number > 0)) {
		ke_mutex_exit(&ep->lock);
		ux_reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);
		return;
	}

	if (br->CONIND_number > 0)
		ep->state = UX_LISTENING;
	else
		ep->state = UX_BOUND;

	ke_mutex_exit(&ep->lock);

	br->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);

	/*
	 * We never deal with the VFS here. SockFS always lowers to rq.
	 * SockFS will, once it receives our T_BIND_ACK, do the VFS binding.
	 */
}

static void
ux_send_conn_ind(ux_endp_t *listener, ux_endp_t *peer, mblk_t *mp)
{
	struct T_conn_ind *ci = (typeof(ci))mp->rptr;
	mp->db->type = M_PROTO;
	ci->PRIM_type = T_CONN_IND;
	ci->SEQ_number = peer->my_seqno;
	/* todo: copy out SRC from our local address */
	mp->wptr += sizeof(*ci);
	str_ingress_putq(listener->rq->stdata, mp);
}

static void
ux_wput_conn_req(queue_t *wq, mblk_t *mp)
{
	ux_endp_t *ep = wq->ptr, *listenerep;
	struct T_conn_req *cr = (typeof(cr))mp->rptr;
	struct sockaddr_ux *sux = (typeof(sux))&cr->DEST;
	mblk_t *cimp;

	cimp = str_allocb(sizeof(struct T_conn_ind));
	if (cimp == NULL) {
		ux_reply_error_ack(wq, mp, T_CONN_REQ, ENOMEM);
		return;
	}

	if (ep->discon_ind_mp == NULL) {
		ep->discon_ind_mp = str_allocb(sizeof(struct T_discon_ind));
		if (ep->discon_ind_mp == NULL) {
			str_freeb(cimp);
			ux_reply_error_ack(wq, mp, T_CONN_REQ, ENOMEM);
			return;
		}
	}

	listenerep = sux->sux_rq->ptr;

	ux_lock_two_eps(ep, listenerep, "ux_wput_conn_req");

	if (ep->state != UX_BOUND && ep->state != UX_UNBOUND) {
		ke_mutex_exit(&ep->lock);
		ke_mutex_exit(&listenerep->lock);
		str_freeb(cimp);
		ux_reply_error_ack(wq, mp, T_CONN_REQ, EINVAL); /* TOUTSTATE */
		return;
	}

	if (listenerep->state != UX_LISTENING || listenerep->closing) {
		ke_mutex_exit(&ep->lock);
		ke_mutex_exit(&listenerep->lock);
		str_freeb(cimp);
		ux_reply_error_ack(wq, mp, T_CONN_REQ,
		    ECONNREFUSED); /* TSYSERR? */
		return;
	}

	ep_retain(ep); /* listenerp's ref on connector */
	LIST_INSERT_HEAD(&listenerep->conninds, ep, conninds_entry);

	ep_retain(listenerep); /* connector's ref on listener */
	ep->connecting_listener = listenerep;
	ep->state = UX_CONNECTING;
	ep->my_seqno = listenerep->next_seqno++;

	ke_mutex_exit(&ep->lock);

	ux_send_conn_ind(listenerep, ep, cimp);
	ke_mutex_exit(&listenerep->lock);

	str_freemsg(mp);
}

static void
ux_send_conn_con(ux_endp_t *listener, ux_endp_t *peer, mblk_t *mp)
{
	struct T_conn_con *cc = (typeof(cc))mp->rptr;
	mp->db->type = M_PROTO;
	cc->PRIM_type = T_CONN_CON;
	cc->RES_length = 0;
	/* todo: copy out RES from our local address */
	cc->RES.ss_family = AF_UX;
	mp->wptr += sizeof(struct T_conn_con);
	str_ingress_putq(peer->rq->stdata, mp);
}

static void
ux_wput_conn_res(queue_t *wq, mblk_t *mp)
{
	queue_t *acceptorrq;
	ux_endp_t *listenerep = wq->ptr, *peerep, *acceptorep;
	struct T_conn_res *cres = (typeof(cres))mp->rptr;
	mblk_t *ccmp, *ccdi;

	/* sockfs/timod contract: won't send this unless this is the case */
	kassert(listenerep->state == UX_LISTENING);

	ke_mutex_enter(&listenerep->lock, "ux_wput_conn_res listenerep");
	LIST_FOREACH(peerep, &listenerep->conninds, conninds_entry)
		if (peerep->my_seqno == cres->SEQ_number)
			break;
	if (peerep != NULL)
		ep_retain(peerep);
	ke_mutex_exit(&listenerep->lock);

	ccmp = str_allocb(sizeof(struct T_conn_con));
	if (ccmp == NULL) {
		ux_reply_error_ack(wq, mp, T_CONN_RES, ENOMEM);
		if (peerep != NULL)
			ep_release(peerep);
		return;
	}

	ccdi = str_allocb(sizeof(struct T_discon_ind));
	if (ccdi == NULL) {
		str_freeb(ccmp);
		ux_reply_error_ack(wq, mp, T_CONN_RES, ENOMEM);
		if (peerep != NULL)
			ep_release(peerep);
		return;
	}

	if (peerep == NULL) {
		ux_reply_error_ack(wq, mp, T_CONN_RES, ECONNABORTED);
		str_freeb(ccmp);
		str_freeb(ccdi);
		return;
	}

	/* sockfs/timod contract: acceptor kept alive + has same type */

	acceptorrq = (queue_t *)cres->ACCEPTOR_id;
	kassert(acceptorrq->qinfo == &ux_rinit);
	acceptorep = acceptorrq->ptr;

	ux_lock_three_eps(listenerep, peerep, acceptorep, "ux_wput_conn_res");

	if ((acceptorep->state != UX_BOUND &&
	    acceptorep->state != UX_UNBOUND) || acceptorep->closing) {
		/* acceptor changed state? */
		ke_mutex_exit(&listenerep->lock);
		ke_mutex_exit(&peerep->lock);
		ke_mutex_exit(&acceptorep->lock);
		ep_release(peerep);
		str_freeb(ccmp);
		str_freeb(ccdi);
		ux_reply_error_ack(wq, mp, T_CONN_RES, EINVAL); /* TBADF? */
		return;
	}

	if (peerep->state != UX_CONNECTING ||
	    peerep->connecting_listener != listenerep) {
		/* connector went away between scan and lock? */
		ke_mutex_exit(&listenerep->lock);
		ke_mutex_exit(&peerep->lock);
		ke_mutex_exit(&acceptorep->lock);
		str_freeb(ccmp);
		str_freeb(ccdi);
		ep_release(peerep);
		ux_reply_error_ack(wq, mp, T_CONN_RES, EINVAL); /* TBADSEQ */
		return;
	}

	/* paranoid check? should be assured by above check */
	{
		ux_endp_t *var;
		LIST_FOREACH(var, &listenerep->conninds, conninds_entry)
			if (var == peerep)
				break;
		if (var == NULL)
			kfatal("check unix implementation");
	}

	LIST_REMOVE(peerep, conninds_entry);

	/* peer should've allocated one at T_CONN_REQ time */
	kassert(peerep->discon_ind_mp != NULL);

	acceptorep->discon_ind_mp = ccdi;

	peerep->connecting_listener = NULL;
	peerep->peer = acceptorep;
	acceptorep->peer = peerep;
	peerep->state = UX_CONNECTED;
	acceptorep->state = UX_CONNECTED;
	ep_retain(acceptorep); /* peer's ref on acceptor */
	ep_retain(peerep); /* acceptor's ref on peer */

	ke_mutex_exit(&listenerep->lock);
	ke_mutex_exit(&acceptorep->lock);

	ux_send_conn_con(listenerep, peerep, ccmp); /* T_CONN_CON to peer */
	ke_mutex_exit(&peerep->lock);

	ep_release(peerep);	/* listenerep conninds' ref on connector */
	ep_release(listenerep); /* peerep's ref on listenerep */

	ux_reply_ok_ack(wq, mp, T_CONN_RES); /* ack to ourselves */

	ep_release(peerep); /* our ref from searching conninds */
}

static void
ux_wput(queue_t *wq, mblk_t *mp)
{
	ux_endp_t *ep = wq->ptr;

	switch (mp->db->type) {
	case M_DATA: {
		ux_endp_t *peerep;

		ke_mutex_enter(&ep->lock, "ux_wput data");
		peerep = ep->peer;
		if (peerep != NULL)
			ep_retain(peerep);
		ke_mutex_exit(&ep->lock);

		if (peerep == NULL) {
			str_freemsg(mp);
			return;
		}

		ke_mutex_enter(&peerep->lock, "ux_wput data peer");
		if (peerep->state != UX_CONNECTED) {
			ke_mutex_exit(&peerep->lock);
			ep_release(peerep);
			str_freemsg(mp);
			return;
		}
		/* if ep is in connected state, it's rq cannot be NULL yet */
		kassert(peerep->rq != NULL);
		str_ingress_putq(peerep->rq->stdata, mp);
		ke_mutex_exit(&peerep->lock);
		ep_release(peerep);

		return;
	}

	case M_PROTO: {
		union T_primitives *prim = (union T_primitives *)mp->rptr;
		switch (prim->type) {
		case T_CONN_REQ:
			ux_wput_conn_req(wq, mp);
			break;

		case T_CONN_RES:
			ux_wput_conn_res(wq, mp);
			break;

		case T_BIND_REQ:
			ux_wput_bind_req(wq, mp);
			break;

		default:
			kfatal("unix_wput: unhandled proto type %d\n",
			    prim->type);
		}

		break;
	}


	default:
		ktodo();
	}
}
