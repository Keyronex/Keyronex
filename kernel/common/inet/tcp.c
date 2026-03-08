/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Jan 09 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file tcp.c
 * @brief Transmission Control Protocol implementation.
 *
 * Notes
 * ------
 *
 * For the sake of timers, tcp_endp::rq can only be written under *both* the
 * endpoint's mutex and detach_lock.
 */

#include <sys/errno.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>

#include <netinet/in.h>
#include <netinet/ip.h>

#include <inet/ip.h>
#include <inet/ip_intf.h>
#include <inet/tcphdr.h>
#include <inet/util.h>

#define TCP_WINDOW	 (536 * 8)
#define TCP_MSS		 536

#define TCP_MAXRETRIES	 5

#define TCP_INIT_RTO_MS	 1000
#define TCP_MAX_RTO_MS	 60000 /* 60 seconds */
#define TCP_MIN_RTO_MS	 200
#define TCP_2MSL_MS	 60000	 /* 60 seconds */
#define TCP_KEEPALIVE_MS 7200000 /* 2 hours */

/* state machine & types */

typedef tcp_seq tcp_seq_t;
#define SEQ_LT(a, b)	((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b)	((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)	((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b)	((int32_t)((a) - (b)) >= 0)

enum tcp_state {
	TCPS_CLOSED = 0,	/* closed */
	TCPS_BOUND,		/* (not rfc) bound, ready to connect/listen */
	TCPS_LISTEN,		/* listening for connection */
	TCPS_SYN_SENT,		/* active open; SYN has been sent */
	TCPS_SYN_RECEIVED,	/* received SYN; awaiting ACK */
	TCPS_ESTABLISHED,	/* established connection */
	TCPS_CLOSE_WAIT,	/* receiver closing; received FIN */
	TCPS_FIN_WAIT_1,	/* initiator closing; sent FIN */
	TCPS_CLOSING,		/* both sides closing simultaneously */
	TCPS_LAST_ACK,		/* receiver closing, sent FIN */
	TCPS_FIN_WAIT_2,	/* initiator closing, received ACK for FIN */
	TCPS_TIME_WAIT,		/* waiting for network to clear (2MSL timer) */
};

enum tcp_timer_type {
	TCP_TIMER_REXMT,
	TCP_TIMER_PERSIST,
	TCP_TIMER_KEEPALIVE,
	TCP_TIMER_2MSL,
	TCP_TIMER_MAX
};

typedef struct tcp {
	kmutex_t	mutex;
	atomic_uint	refcnt;

	kspinlock_t	detach_lock;	/* involved in detaching */
	queue_t		*rq;		/* stream read queue, if not detached */

	TAILQ_ENTRY(tcp) detached_link; /* in tcp_detached_work_q */
	bool	on_detached_work_q; /* is it currently on detached work q? */

	/* in detached state, our send queue */
	mblk_q_t detached_snd_q;
	uint32_t detached_snd_q_count;

	/* in detached state, our receive queue */
	/* TODO could merge with above? detached close = hard reset on recv */
	mblk_q_t detached_rcv_q;
	uint32_t detached_rcv_q_count;

	/* listener / passive-open bookkeeping */
	bool	closing;
	LIST_HEAD(, tcp) conninds;	/* valid while LISTEN */
	uint32_t next_connind_seq;	/* valid while LISTEN */

	LIST_ENTRY(tcp) conninds_entry;	/* valid for passive child */
	struct tcp *passive_listener;	/* retained while queued */
	uint32_t connind_seq;		/* T_CONN_IND / T_DISCON_IND seqno */
	bool	connind_sent;	 	/* T_CONN_IND already sent upward */

	enum tcp_state state;	/* state of TCB */
	int conn_id;		/* ID in the connections/binds table */

	struct sockaddr_in laddr;	/* local address */
	struct sockaddr_in faddr;	/* foreign address */

	ip_intf_t *processing_ifp;	/* ifp context we are executing in
					 * right now, if in it*/

	struct tcp_timer {
		kcallout_t	callout;
		kdpc_t		dpc;
		kabstime_t	deadline;
	} timers[TCP_TIMER_MAX];	/* TCP timers */
	atomic_uint pending_timers;	/* TCP timers that fired */

	bool	shutdown_rd:	1, /* can't receive any more. */
		emit_ack: 	1, /* send ACK next tcp_output() */
		ordrel_needed:	1; /* need to put up T_ORDREL_IND after data */

	uint16_t	mss;

	tcp_seq_t	iss;		/* initial send sequence number */
	tcp_seq_t	snd_una;	/* send unacknowledged */
	tcp_seq_t	snd_nxt;	/* send next (BSD meaning, not RFC) */
	tcp_seq_t	snd_max;	/* highest sent (like RFC SND.NXT) */
	uint32_t	snd_wnd;	/* send window */
	tcp_seq_t	snd_up;		/* send urgent pointer */
	tcp_seq_t	snd_wl1;	/* last window update segment seq */
	tcp_seq_t	snd_wl2;	/* last window update segment ack */

	tcp_seq_t	irs;		/* initial receive sequence number */
	tcp_seq_t	rcv_nxt;	/* receive next */
	uint32_t	rcv_wnd;	/* receive window */
	tcp_seq_t	rcv_up; 	/* receive urgent pointer */
	tcp_seq_t	rcv_wnd_max;	/* maximum receive window */

	bool	timing_rtt;	/* is RTT being timed for a transmitted seg? */
	tcp_seq_t	rtseq;		/* seq of segment being timed */
	kabstime_t	rtstart;	/* time RTT measurement began */

	uint32_t	srtt;		/* smoothed RTT (ms) */
	uint32_t	rttvar;		/* RTT variation (ms) */
	uint32_t	rto;		/* retransmission timeout (ms) */
	uint8_t		n_rexmits;	/* N rexmits of current segment */

	mblk_q_t	reass_queue;	/* segment reassembly queue */

	/* mblks preallocated for connected TCBs. */
	mblk_t	*conn_con_m;	/* preallocated T_CONN_CON */
	mblk_t	*ordrel_ind_m;	/* preallocated T_ORDREL_IND */
	mblk_t	*discon_ind_m;	/* preallocated T_DISCON_IND */

	/* mblk preallocated for TCBs created by passive opens */
	mblk_t	*conn_ind_m;	/* preallocated T_CONN_IND */

	krcu_entry_t rcu;		/* for RCU deferred free'ing */
} tcp_t;

static int tcp_open(queue_t *, void *dev);
static void tcp_close(queue_t *);
static void tcp_wput(queue_t *, mblk_t *);
static void tcp_wsrv(queue_t *);
static void tcp_rput(queue_t *, mblk_t *);
static void tcp_rsrv(queue_t *);

static int tcp_output(tcp_t *);

static void tcp_ordrel_ind(tcp_t *tp);
void tcp_conn_input(ip_intf_t *ifp, tcp_t **tpp, mblk_t *);

static void tcp_rexmt_timer(tcp_t *);
static void tcp_2msl_timer(tcp_t *);
static void tcp_keepalive_timer(tcp_t *);
static void tcp_persist_timer(tcp_t *);

void tcp_set_timer(tcp_t *, enum tcp_timer_type, uint32_t timeout_ms);
void tcp_cancel_timer(tcp_t *, enum tcp_timer_type);
void tcp_cancel_all_timers(tcp_t *);
static void tcp_timer_dpchandler(void *, void *);

struct qinit tcp_rinit = {
	.qopen = tcp_open,
	.qclose = tcp_close,
	.putp = tcp_rput,
	.srvp = tcp_rsrv,
};

static struct qinit tcp_winit = {
	.putp = tcp_wput,
	.srvp = tcp_wsrv,
};

struct streamtab tcp_streamtab = {
	.rinit = &tcp_rinit,
	.winit = &tcp_winit,
};

static const char *tcp_state_names[] = {
	[TCPS_CLOSED] = "CLOSED",
	[TCPS_BOUND] = "BOUND",
	[TCPS_LISTEN] = "LISTEN",
	[TCPS_SYN_SENT] = "SYN_SENT",
	[TCPS_SYN_RECEIVED] = "SYN-RECEIVED",
	[TCPS_ESTABLISHED] = "ESTABLISHED",
	[TCPS_CLOSE_WAIT] = "CLOSE-WAIT",
	[TCPS_FIN_WAIT_1] = "FIN-WAIT-1",
	[TCPS_CLOSING] = "CLOSING",
	[TCPS_LAST_ACK] = "LAST-ACK",
	[TCPS_FIN_WAIT_2] = "FIN-WAIT-2",
	[TCPS_TIME_WAIT] = "TIME-WAIT",
};

#define TCP_CONN_TABLE_SIZE 256
static tcp_t *tcp_conn_table[TCP_CONN_TABLE_SIZE];
static krwlock_t tcp_conntab_lock;

#define TCP_EPHEMERAL_LOW  49152
#define TCP_EPHEMERAL_HIGH 65535
static uint16_t tcp_next_ephemeral = TCP_EPHEMERAL_LOW;
static krwlock_t tcp_bind_lock;

static kspinlock_t tcp_detached_lock;
static TAILQ_HEAD(, tcp) tcp_detached_work_q =
    TAILQ_HEAD_INITIALIZER(tcp_detached_work_q);
static kevent_t tcp_detached_event;

#define TCP_TRACE(...) kdprintf("TCP: " __VA_ARGS__)

static tcp_t *
tcp_new(queue_t *rq)
{
	tcp_t *tp = kmem_alloc(sizeof(*tp));
	if (tp == NULL)
		return NULL;

	ke_mutex_init(&tp->mutex);
	tp->refcnt = 1;

	tp->on_detached_work_q = false;

	tp->state = TCPS_CLOSED;
	tp->conn_id = -1;

	tp->closing = false;
	LIST_INIT(&tp->conninds);
	tp->next_connind_seq = 1;
	tp->passive_listener = NULL;
	tp->connind_seq = 0;
	tp->connind_sent = false;

	ke_spinlock_init(&tp->detach_lock);

	tp->rq = rq;

	tp->processing_ifp = NULL;

	tp->mss = TCP_MSS;

	tp->iss = 0;
	tp->snd_una = 0;
	tp->snd_nxt = 0;
	tp->snd_wnd = 0;
	tp->snd_max = 0;

	tp->irs = 0;
	tp->rcv_nxt = 0;
	tp->rcv_wnd = TCP_WINDOW;
	tp->rcv_wnd_max = TCP_WINDOW;

	tp->timing_rtt = false;
	tp->srtt = 0;
	tp->rttvar = 0;
	tp->rto = TCP_INIT_RTO_MS;
	tp->n_rexmits = 0;

	tp->conn_con_m = NULL;
	tp->ordrel_ind_m = NULL;
	tp->discon_ind_m = NULL;

	tp->conn_ind_m = NULL;

	tp->shutdown_rd = 0;
	tp->emit_ack = 0;
	tp->ordrel_needed = 0;

	TAILQ_INIT(&tp->detached_snd_q);
	tp->detached_snd_q_count = 0;

	TAILQ_INIT(&tp->detached_rcv_q);
	tp->detached_rcv_q_count = 0;

	for (int i = 0; i < TCP_TIMER_MAX; i++) {
		ke_callout_init_dpc(&tp->timers[i].callout, &tp->timers[i].dpc,
		    tcp_timer_dpchandler, tp, &tp->timers[i]);
		tp->timers[i].deadline = ABSTIME_NEVER;
	}
	tp->pending_timers = 0;

	TAILQ_INIT(&tp->reass_queue);

	return tp;
}

static void
tcp_retain(tcp_t *tp)
{
	atomic_fetch_add_explicit(&tp->refcnt, 1, memory_order_relaxed);
}

static bool
tcp_tryretain(tcp_t *tp)
{
	ipl_t ipl = spldisp();
	unsigned int current = atomic_load_explicit(&tp->refcnt,
	    memory_order_acquire);
	for (;;) {
		if (current == 0) {
			splx(ipl);
			return false;
		}
		if (atomic_compare_exchange_weak_explicit(&tp->refcnt, &current,
			current + 1, memory_order_acq_rel,
			memory_order_acquire)) {
			splx(ipl);
			return true;
		}
	}
}

static void
tcp_free_rcu(void *arg)
{
	tcp_t *tp = arg;
	kmem_free(tp, sizeof(*tp));
}

static void
tcp_release(tcp_t *tp)
{
	if (atomic_fetch_sub_explicit(&tp->refcnt, 1, memory_order_acq_rel) ==
	    1) {
		kassert(tp->rq == NULL);
		tcp_cancel_all_timers(tp);
		kdprintf("rcu-freeing TCB %p\n", tp);
		/* RCU free since we may have DPCs in flight */
		ke_rcu_call(&tp->rcu, tcp_free_rcu, tp);
	}
}

static inline void
tcp_swap_tcbs(tcp_t **a, tcp_t **b)
{
	tcp_t *t = *a;
	*a = *b;
	*b = t;
}

static void
tcp_lock_two_tcbs(tcp_t *a, tcp_t *b, const char *why)
{
	if (a < b) {
		ke_mutex_enter(&a->mutex, why);
		ke_mutex_enter(&b->mutex, why);
	} else if (a > b) {
		ke_mutex_enter(&b->mutex, why);
		ke_mutex_enter(&a->mutex, why);
	} else {
		kfatal("tcp_lock_two_tcbs: identical TCBs");
	}
}

static void
tcp_lock_three_tcbs(tcp_t *a, tcp_t *b, tcp_t *c, const char *why)
{
	tcp_t *v[3] = { a, b, c };

	if (v[0] > v[1])
		tcp_swap_tcbs(&v[0], &v[1]);
	if (v[1] > v[2])
		tcp_swap_tcbs(&v[1], &v[2]);
	if (v[0] > v[1])
		tcp_swap_tcbs(&v[0], &v[1]);

	if (v[0] == v[1] || v[1] == v[2])
		kfatal("tcp_lock_three_tcbs: identical TCBs");

	ke_mutex_enter(&v[0]->mutex, why);
	ke_mutex_enter(&v[1]->mutex, why);
	ke_mutex_enter(&v[2]->mutex, why);
}


static void
tcp_change_state(tcp_t *tp, enum tcp_state newstate)
{
	TCP_TRACE("TCB %p state change %s -> %s\n", tp,
	    tcp_state_names[tp->state], tcp_state_names[newstate]);
	tp->state = newstate;
}

static void
tcb_free_connstate(tcp_t *tp)
{
	kassert(tp->passive_listener == NULL);
	if (tp->state == TCPS_LISTEN)
		kassert(LIST_EMPTY(&tp->conninds));

	tcp_change_state(tp, TCPS_CLOSED);
	tp->closing = false;

	tcp_cancel_all_timers(tp);

	if (tp->conn_id != -1) {
		ke_rwlock_enter_write(&tcp_conntab_lock, "tcp_free_connstate");
		kassert(tcp_conn_table[tp->conn_id] == tp);
		tcp_conn_table[tp->conn_id] = NULL;
		ke_rwlock_exit_write(&tcp_conntab_lock);
		tp->conn_id = -1;
	}

	str_mblk_q_free(&tp->reass_queue);

	str_mblk_q_free(&tp->detached_snd_q);
	tp->detached_snd_q_count = 0;

	str_mblk_q_free(&tp->detached_rcv_q);
	tp->detached_rcv_q_count = 0;

	str_freeb(tp->conn_con_m);
	tp->conn_con_m = NULL;
	str_freeb(tp->ordrel_ind_m);
	tp->ordrel_ind_m = NULL;
	str_freeb(tp->discon_ind_m);
	tp->discon_ind_m = NULL;
	str_freeb(tp->conn_ind_m);
	tp->conn_ind_m = NULL;

	tp->connind_sent = false;
	tp->connind_seq = 0;
}

static queue_t *
tcp_wq(tcp_t *tcb)
{
	return tcb->rq != NULL ? tcb->rq->other : NULL;
}

static void
tcp_detach(tcp_t *tp)
{
	ipl_t ipl;
	uint32_t pending;

	/* steal the send queue */
	TAILQ_CONCAT(&tp->detached_snd_q, &tcp_wq(tp)->msgq, link);
	tp->detached_snd_q_count = tcp_wq(tp)->count;

	tcp_wq(tp)->count = 0;
	TAILQ_INIT(&tcp_wq(tp)->msgq);

	ipl = ke_spinlock_enter(&tp->detach_lock);
	tp->rq = NULL;
	ke_spinlock_exit(&tp->detach_lock, ipl);

	pending = atomic_load(&tp->pending_timers);
	if (pending != 0) {
		tcp_retain(tp); /* safe, we are certainly alive */
		ipl = ke_spinlock_enter(&tcp_detached_lock);
		TAILQ_INSERT_TAIL(&tcp_detached_work_q, tp, detached_link);
		ke_event_set_signalled(&tcp_detached_event, true);
		ke_spinlock_exit(&tcp_detached_lock, ipl);
	}

	/*
	 * FIXME: what if there are packets on the stream ingress queue?
	 * it is not big problem (they'll get retransmitted by remote) but not
	 * desirable.
	 * we might legitimately NOT use the ingress queue at all even when
	 * attached, but instead queue up packets on a queue in the TCB, and
	 * have tcp_rsrv() deal with them.
	 * Has flow control implications but the ingress queue doesn't deal
	 * with those at all right now anyway.
	 */
}

static void
tcp_attach(tcp_t *child, tcp_t *acceptor)
{
	queue_t *rq;
	ipl_t ipl;

	kassert(ke_mutex_held(&child->mutex));
	kassert(ke_mutex_held(&acceptor->mutex));
	kassert(child->rq == NULL);
	kassert(acceptor->rq != NULL);

	rq = acceptor->rq;

	kassert(rq->ptr == acceptor);
	kassert(rq->other->ptr == acceptor);
	kassert(TAILQ_EMPTY(&rq->msgq));
	kassert(TAILQ_EMPTY(&child->detached_snd_q));

	ipl = ke_spinlock_enter(&acceptor->detach_lock);
	acceptor->rq = NULL;
	ke_spinlock_exit(&acceptor->detach_lock, ipl);

	rq->ptr = rq->other->ptr = child;

	ipl = ke_spinlock_enter(&child->detach_lock);
	child->rq = rq;
	ke_spinlock_exit(&child->detach_lock, ipl);

	if (!TAILQ_EMPTY(&child->detached_rcv_q)) {
		TAILQ_CONCAT(&rq->msgq, &child->detached_rcv_q, link);
		rq->count += child->detached_rcv_q_count;
		TAILQ_INIT(&child->detached_rcv_q);
		child->detached_rcv_q_count = 0;
		str_qenable(rq);
	}

	if (child->ordrel_needed && TAILQ_EMPTY(&rq->msgq)) {
		child->ordrel_needed = false;
		tcp_ordrel_ind(child);
	}

	if (atomic_load(&child->pending_timers) != 0)
		str_qenable(rq->other);

	str_kick(rq->stdata);
}

static int
tcp_open(queue_t *rq, void *)
{
	tcp_t *tp = tcp_new(rq);
	if (tp == NULL)
		return -ENOMEM;
	rq->ptr = rq->other->ptr = tp;
	return 0;
}

static void
tcp_passive_unlink(tcp_t *listener, tcp_t *child)
{
	kassert(ke_mutex_held(&listener->mutex));
	kassert(ke_mutex_held(&child->mutex));
	kassert(child->passive_listener == listener);

	LIST_REMOVE(child, conninds_entry);
	child->passive_listener = NULL;

	tcp_release(child);	/* listener list ref on child */
	tcp_release(listener); /* child ref on listener */
}

static void
tcp_close(queue_t *rq)
{
	tcp_t *tp = rq->ptr;

	ke_mutex_enter(&tp->mutex, "tcp_close");

	TCP_TRACE("closing connection TCB %p state %s\n", tp,
	    tcp_state_names[tp->state]);

	switch (tp->state) {
	case TCPS_LISTEN: {
		for (;;) {
			tcp_t *child;

			if (tp->conn_id != -1) {
				ke_rwlock_enter_write(&tcp_conntab_lock,
				    "tcp_close listen remove");
				if (tcp_conn_table[tp->conn_id] == tp)
					tcp_conn_table[tp->conn_id] = NULL;
				ke_rwlock_exit_write(&tcp_conntab_lock);
				tp->conn_id = -1;
			}

			tp->closing = true;

			child = LIST_FIRST(&tp->conninds);
			if (child == NULL)
				break;

			tcp_retain(child); /* local ref */

			if (tp > child) {
				ke_mutex_exit(&tp->mutex);
				ke_mutex_enter(&child->mutex, "tcp_close listen child");
				ke_mutex_enter(&tp->mutex, "tcp_close listen relock");
			} else {
				ke_mutex_enter(&child->mutex, "tcp_close listen child");
			}

			if (child->passive_listener == tp) {
				tcp_passive_unlink(tp, child);
				tcb_free_connstate(child);
				ke_mutex_exit(&child->mutex);
				tcp_release(child); /* child existence ref */
			} else {
				ke_mutex_exit(&child->mutex);
			}

			tcp_release(child); /* local ref */
		}

		tcp_detach(tp);
		tcb_free_connstate(tp);
		ke_mutex_exit(&tp->mutex);
		tcp_release(tp);
		break;
	}

	case TCPS_CLOSED:
	case TCPS_BOUND:
	case TCPS_SYN_SENT:
		tcp_detach(tp);
		tcb_free_connstate(tp);
		ke_mutex_exit(&tp->mutex);
		tcp_release(tp);
		break;

	case TCPS_SYN_RECEIVED:
	case TCPS_ESTABLISHED:
		if (!TAILQ_EMPTY(&tp->reass_queue) ||
		    !TAILQ_EMPTY(&tp->rq->msgq))
			kfatal("TODO: tcp_close: unreceived data on close\n");

		tcp_detach(tp);
		tcp_change_state(tp, TCPS_FIN_WAIT_1);
		tcp_output(tp);
		ke_mutex_exit(&tp->mutex);
		break;

	case TCPS_CLOSE_WAIT:
		if (!TAILQ_EMPTY(&tp->reass_queue) ||
		    !TAILQ_EMPTY(&tp->rq->msgq))
			kfatal("TODO: tcp_close: unreceived data on close\n");

		tcp_detach(tp);
		tcp_change_state(tp, TCPS_LAST_ACK);
		tcp_output(tp);
		ke_mutex_exit(&tp->mutex);
		break;

	case TCPS_FIN_WAIT_1:
	case TCPS_CLOSING:
	case TCPS_LAST_ACK:
	case TCPS_FIN_WAIT_2:
	case TCPS_TIME_WAIT:
		kassert(TAILQ_EMPTY(&tp->reass_queue) &&
		    TAILQ_EMPTY(&tp->rq->msgq));
		tcp_detach(tp);
		ke_mutex_exit(&tp->mutex);
		break;
	}
}

static size_t
tcp_snd_q_count(tcp_t *tp)
{
	if (tp->rq == NULL)
		return tp->detached_snd_q_count;
	else
		return tp->rq->other->count;
}

/*
 * connection/binding managmeent table
 */

static bool
tcp_port_in_use(uint16_t port, in_addr_t addr)
{
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL && t->laddr.sin_port == port &&
		    (t->laddr.sin_addr.s_addr == INADDR_ANY ||
			addr == INADDR_ANY || t->laddr.sin_addr.s_addr == addr))
			return true;
	}
	return false;
}

static uint16_t
tcp_alloc_ephemeral(in_addr_t addr)
{
	uint16_t start = tcp_next_ephemeral;

	do {
		uint16_t port = htons(tcp_next_ephemeral);
		tcp_next_ephemeral++;
		if (tcp_next_ephemeral > TCP_EPHEMERAL_HIGH)
			tcp_next_ephemeral = TCP_EPHEMERAL_LOW;

		if (!tcp_port_in_use(port, addr))
			return port;
	} while (tcp_next_ephemeral != start);

	return 0;
}

static int
tcp_conn_table_insert(tcp_t *tp)
{
	ke_rwlock_enter_write(&tcp_conntab_lock, "tcp_conn_table_insert");
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		if (tcp_conn_table[i] == NULL) {
			tcp_conn_table[i] = tp;
			tp->conn_id = i;
			ke_rwlock_exit_write(&tcp_conntab_lock);
			return 0;
		}
	}
	ke_rwlock_exit_write(&tcp_conntab_lock);
	return -ENOSPC;
}

static tcp_t *
tcp_conn_lookup(struct in_addr src, uint16_t sport, struct in_addr dst,
    uint16_t dport)
{
	ke_rwlock_enter_read(&tcp_conntab_lock, "tcp_conn_lookup");
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL && t->laddr.sin_addr.s_addr == dst.s_addr &&
		    t->laddr.sin_port == dport &&
		    t->faddr.sin_addr.s_addr == src.s_addr &&
		    t->faddr.sin_port == sport) {
			tcp_retain(t);
			ke_rwlock_exit_read(&tcp_conntab_lock);
			return t;
		}
	}

	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL && t->state == TCPS_LISTEN &&
		    t->laddr.sin_port == dport &&
		    (t->laddr.sin_addr.s_addr == INADDR_ANY ||
			t->laddr.sin_addr.s_addr == dst.s_addr)) {
			tcp_retain(t);
			ke_rwlock_exit_read(&tcp_conntab_lock);
			return t;
		}
	}

	ke_rwlock_exit_read(&tcp_conntab_lock);
	return NULL;
}

/*
 * output processing
 */
static void
reply_ok_ack(queue_t *wq, mblk_t *mp, enum T_prim correct_prim)
{
	struct T_ok_ack *oa = (struct T_ok_ack *)mp->rptr;
	kassert(mp->wptr - mp->rptr >= sizeof(*oa));
	oa->PRIM_type = T_OK_ACK;
	oa->CORRECT_prim = correct_prim;
	mp->wptr = mp->rptr + sizeof(*oa);
	str_qreply(wq, mp);
}

static void
reply_error_ack(queue_t *wq, mblk_t *mp, enum T_prim error_prim,
    int unix_error)
{
	struct T_error_ack *ea = (struct T_error_ack *)mp->rptr;
	kassert(mp->wptr - mp->rptr >= sizeof(*ea));
	ea->PRIM_type = T_ERROR_ACK;
	ea->ERROR_prim = error_prim;
	ea->UNIX_error = unix_error;
	mp->wptr = mp->rptr + sizeof(*ea);
	str_qreply(wq, mp);
}

static int
tcp_do_bind(tcp_t *tp, struct sockaddr_in *laddr)
{
	uint16_t port;
	int r;

	ke_rwlock_enter_write(&tcp_bind_lock, "tcp_do_bind");

	if (laddr->sin_port == 0) {
		port = tcp_alloc_ephemeral(laddr->sin_addr.s_addr);
		if (port == 0) {
			ke_rwlock_exit_write(&tcp_bind_lock);
			return -EADDRINUSE;
		}
	} else {
		if (tcp_port_in_use(laddr->sin_port, laddr->sin_addr.s_addr)) {
			ke_rwlock_exit_write(&tcp_bind_lock);
			return -EADDRINUSE;
		}
		port = laddr->sin_port;
	}

	tp->laddr.sin_family = AF_INET;
	tp->laddr.sin_port = port;
	tp->laddr.sin_addr = laddr->sin_addr;

	r = tcp_conn_table_insert(tp);
	if (r != 0) {
		ke_rwlock_exit_write(&tcp_bind_lock);
		return r;
	}

	tcp_change_state(tp, TCPS_BOUND);

	ke_rwlock_exit_write(&tcp_bind_lock);
	return 0;
}

static tcp_seq_t
tcp_issgen(void)
{
	return ke_time() / 4000;
}

static int
tcp_setup_connection(tcp_t *tp, struct sockaddr_in *faddr)
{
	tp->faddr = *faddr;

	tp->iss = tcp_issgen();
	tp->snd_una = tp->iss;
	tp->snd_nxt = tp->iss;
	tp->snd_max = tp->iss;
	tp->snd_wnd = TCP_WINDOW;
	tp->snd_wl1 = 0;
	tp->snd_wl2 = 0;

	tp->emit_ack = false;

	tp->conn_con_m = str_allocb(sizeof(struct T_conn_con));
	tp->ordrel_ind_m = str_allocb(sizeof(struct T_ordrel_ind));
	tp->discon_ind_m = str_allocb(sizeof(struct T_discon_ind));
	if (tp->conn_con_m == NULL || tp->ordrel_ind_m == NULL ||
	    tp->discon_ind_m == NULL) {
		str_freeb(tp->conn_con_m);
		tp->conn_con_m = NULL;
		str_freeb(tp->ordrel_ind_m);
		tp->ordrel_ind_m = NULL;
		str_freeb(tp->discon_ind_m);
		tp->discon_ind_m = NULL;
		return -ENOMEM;
	}

	tp->conn_con_m->db->type = M_PROTO;
	tp->conn_con_m->wptr += sizeof(struct T_conn_con);
	tp->ordrel_ind_m->db->type = M_PROTO;
	tp->ordrel_ind_m->wptr += sizeof(struct T_ordrel_ind);
	tp->discon_ind_m->db->type = M_PROTO;
	tp->discon_ind_m->wptr += sizeof(struct T_discon_ind);

	return 0;
}

static void
tcp_wput_bind_req(queue_t *wq, mblk_t *mp)
{
	tcp_t *tp = wq->ptr;
	struct T_bind_req *br = (struct T_bind_req *)mp->rptr;
	struct sockaddr_in sin;
	int r;

	ke_mutex_enter(&tp->mutex, "tcp_wput_bind_req");

	if (!(tp->state == TCPS_CLOSED) &&
	    !(tp->state == TCPS_BOUND && br->CONIND_number > 0)) {
		ke_mutex_exit(&tp->mutex);
		reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);
		return;
	}

	if (br->ADDR_length >= sizeof(struct sockaddr_in)) {
		sin = *(struct sockaddr_in *)&br->ADDR;
	} else {
		sin.sin_family = AF_INET;
		sin.sin_port = 0;
		sin.sin_addr.s_addr = INADDR_ANY;
	}

	if (tp->state == TCPS_CLOSED) {
		r = tcp_do_bind(tp, &sin);
		if (r != 0) {
			ke_mutex_exit(&tp->mutex);
			reply_error_ack(wq, mp, T_BIND_REQ, -r);
			return;
		}
	}

	if (br->CONIND_number > 0) {
		tp->closing = false;
		tcp_change_state(tp, TCPS_LISTEN);
	} else {
		tcp_change_state(tp, TCPS_BOUND);
	}

	ke_mutex_exit(&tp->mutex);

	br->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);
}

static void
tcp_wput_conn_req(queue_t *wq, mblk_t *mp)
{
	struct T_conn_req *cr = (struct T_conn_req *)mp->rptr;
	struct sockaddr_in *dest = (struct sockaddr_in *)&cr->DEST;
	tcp_t *tp = wq->ptr;
	int r;

	ke_mutex_enter(&tp->mutex, "tcp_wput_conn_req");

	switch (tp->state) {
	case TCPS_CLOSED: {
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_port = 0,
			.sin_addr.s_addr = INADDR_ANY,
		};

		r = tcp_do_bind(tp, &sin);
		if (r != 0) {
			ke_mutex_exit(&tp->mutex);
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}

		/* fall through... */
	}

	case TCPS_BOUND:
		if (tp->laddr.sin_addr.s_addr == INADDR_ANY) {
			struct ip_route_result rt;

			rt = ip_route_lookup(dest->sin_addr);
			if (rt.intf == NULL) {
				ke_mutex_exit(&tp->mutex);
				reply_error_ack(wq, mp, T_CONN_REQ,
				    EHOSTUNREACH);
				return;
			}

			tp->laddr.sin_addr = rt.intf->addr;
			tp->mss = rt.intf->mtu - sizeof(struct ip) -
			    sizeof(struct tcphdr);
			ip_intf_release(rt.intf);
		}

		r = tcp_setup_connection(tp, dest);
		if (r != 0) {
			ke_mutex_exit(&tp->mutex);
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}

		tcp_change_state(tp, TCPS_SYN_SENT);
		r = tcp_output(tp);
		if (r != 0) {
			/* TODO: cleanup state? */
			ke_mutex_exit(&tp->mutex);
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}

		reply_ok_ack(wq, mp, T_CONN_REQ);
		break;

	default:
		TCP_TRACE("tcp_connect: invalid state %d\n", tp->state);
		reply_error_ack(wq, mp, T_CONN_REQ, EISCONN);
		return;
	}
}

static void
tcp_wput_conn_res(queue_t *wq, mblk_t *mp)
{
	tcp_t *listener = wq->ptr;
	struct T_conn_res *cres = (struct T_conn_res *)mp->rptr;
	queue_t *acceptorrq;
	tcp_t *child = NULL, *acceptor;
	bool ok = false;

	acceptorrq = (queue_t *)cres->ACCEPTOR_id;
	kassert(acceptorrq->qinfo == &tcp_rinit);

	ke_mutex_enter(&listener->mutex, "tcp_wput_conn_res listener");
	if (listener->state != TCPS_LISTEN || listener->closing) {
		ke_mutex_exit(&listener->mutex);
		reply_error_ack(wq, mp, T_CONN_RES, EINVAL);
		return;
	}

	LIST_FOREACH(child, &listener->conninds, conninds_entry)
		if (child->connind_seq == cres->SEQ_number)
			break;
	if (child != NULL)
		tcp_retain(child); /* local ref */

	ke_mutex_exit(&listener->mutex);

	if (child == NULL) {
		reply_error_ack(wq, mp, T_CONN_RES, ECONNABORTED);
		return;
	}

	acceptor = acceptorrq->ptr;

	/* no self accept yet */
	if (acceptor == listener) {
		tcp_release(child);
		reply_error_ack(wq, mp, T_CONN_RES, EINVAL);
		return;
	}

	tcp_lock_three_tcbs(listener, child, acceptor, "tcp_wput_conn_res");

	if (listener->state != TCPS_LISTEN || listener->closing)
		goto out;
	if (child->passive_listener != listener)
		goto out;
	if (child->state < TCPS_ESTABLISHED || !child->connind_sent)
		goto out;

	if ((acceptor->state != TCPS_CLOSED && acceptor->state != TCPS_BOUND) ||
	    acceptor->rq == NULL)
		goto out;

	kassert(TAILQ_EMPTY(&acceptor->reass_queue));
	kassert(TAILQ_EMPTY(&acceptor->detached_snd_q));
	kassert(TAILQ_EMPTY(&acceptor->detached_rcv_q));

	tcp_passive_unlink(listener, child);

	tcb_free_connstate(acceptor);
	tcp_attach(child, acceptor);

	ok = true;

out:
	ke_mutex_exit(&listener->mutex);
	ke_mutex_exit(&child->mutex);
	ke_mutex_exit(&acceptor->mutex);

	if (ok) {
		tcp_release(acceptor); /* drop old acceptor ref from q->ptr */
		tcp_release(child);    /* drop local ref from search */
		reply_ok_ack(wq, mp, T_CONN_RES);
	} else {
		tcp_release(child);
		reply_error_ack(wq, mp, T_CONN_RES, EINVAL);
	}
}

static void
tcp_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_DATA:
		str_putq(wq, mp);
		tcp_output((tcp_t*)wq->ptr);
		break;

	case M_PROTO: {
		union T_primitives *prim = (union T_primitives *)mp->rptr;
		switch (prim->type) {
		case T_CONN_REQ:
			tcp_wput_conn_req(wq, mp);
			break;

		case T_CONN_RES:
			tcp_wput_conn_res(wq, mp);
			break;

		case T_BIND_REQ:
			tcp_wput_bind_req(wq, mp);
			break;

		default:
			kfatal("tcp_wput: unhandled proto type %d\n",
			    prim->type);
		}
		break;
	}

	default:
		kfatal("tcp_wput: unhandled mblk type %d\n", mp->db->type);
	}
}

static void
tcp_wsrv(queue_t *wq)
{
	tcp_t *tp = wq->ptr;
	uint32_t pending = atomic_exchange(&tp->pending_timers, 0);
	if (pending != 0) {
		ke_mutex_enter(&tp->mutex, "tcp_rsrv_timers");
		if (pending & (1 << TCP_TIMER_REXMT))
			tcp_rexmt_timer(tp);
		if (pending & (1 << TCP_TIMER_2MSL))
			tcp_2msl_timer(tp);
		if (pending & (1 << TCP_TIMER_KEEPALIVE))
			tcp_keepalive_timer(tp);
		if (pending & (1 << TCP_TIMER_PERSIST))
			tcp_persist_timer(tp);
		ke_mutex_exit(&tp->mutex);
	}
}

static uint8_t tcp_out_flags[] = {
	[TCPS_CLOSED] = TH_RST | TH_ACK,
	[TCPS_BOUND] = TH_RST | TH_ACK,
	[TCPS_LISTEN] = 0,
	[TCPS_SYN_SENT] = TH_SYN,
	[TCPS_SYN_RECEIVED] = TH_SYN | TH_ACK,
	[TCPS_ESTABLISHED] = TH_ACK,
	[TCPS_CLOSE_WAIT] = TH_ACK,
	[TCPS_FIN_WAIT_1] = TH_FIN | TH_ACK,
	[TCPS_CLOSING] = TH_FIN | TH_ACK,
	[TCPS_LAST_ACK] = TH_FIN | TH_ACK,
	[TCPS_FIN_WAIT_2] = TH_ACK,
	[TCPS_TIME_WAIT] = TH_ACK,
};

static void
copy_data(tcp_t *tcb, size_t off, size_t len, uint8_t *dst)
{
	mblk_q_t *q;
	mblk_t *mblk;
	size_t current_offset = 0;
	size_t bytes_copied = 0;

	if (len == 0 || dst == NULL)
		return;

	if (tcp_wq(tcb) != NULL)
		q = &tcp_wq(tcb)->msgq;
	else
		q = &tcb->detached_snd_q;

	TAILQ_FOREACH(mblk, q, link) {
		size_t msgsize = mblk->wptr - mblk->rptr;

		if (current_offset + msgsize > off) {
			size_t block_offset = off - current_offset;
			size_t available = msgsize - block_offset;
			size_t to_copy = MIN2(available, len);

			memcpy(dst, mblk->rptr + block_offset,
			    to_copy);
			bytes_copied += to_copy;

			if (bytes_copied == len)
				return;

			mblk = TAILQ_NEXT(mblk, link);
			break;
		}

		current_offset += msgsize;
	}

	/* offset out of range, or queue exhausted */
	if (mblk == NULL)
		return;

	while (mblk != NULL && bytes_copied < len) {
		size_t msgsize = mblk->wptr - mblk->rptr;
		size_t to_copy = MIN2(msgsize, len - bytes_copied);
		memcpy(dst + bytes_copied, mblk->rptr, to_copy);
		bytes_copied += to_copy;
		mblk = TAILQ_NEXT(mblk, link);
	}
}

static uint32_t
csum_add(uint32_t sum, const uint8_t *p, size_t len)
{
	while (len >= 2) {
		sum += ((uint16_t)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (uint16_t)p[0] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return sum;
}

static uint16_t
csum_finish(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (uint16_t)~sum;
}

uint16_t
tcp_checksum(struct ip *ip, struct tcphdr *tcp, size_t tcp_len)
{
	uint8_t pseudo[12];
	uint32_t sum = 0;

	pseudo[0] = (ip->ip_src.s_addr >> 24) & 0xff;
	pseudo[1] = (ip->ip_src.s_addr >> 16) & 0xff;
	pseudo[2] = (ip->ip_src.s_addr >> 8) & 0xff;
	pseudo[3] = (ip->ip_src.s_addr >> 0) & 0xff;
	pseudo[4] = (ip->ip_dst.s_addr >> 24) & 0xff;
	pseudo[5] = (ip->ip_dst.s_addr >> 16) & 0xff;
	pseudo[6] = (ip->ip_dst.s_addr >> 8) & 0xff;
	pseudo[7] = (ip->ip_dst.s_addr >> 0) & 0xff;
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_TCP;
	pseudo[10] = (tcp_len >> 8) & 0xff;
	pseudo[11] = tcp_len & 0xff;

	sum = csum_add(sum, pseudo, sizeof(pseudo));
	sum = csum_add(sum, (const uint8_t *)tcp, tcp_len);

	return csum_finish(sum);
}

static int
do_send(tcp_t *tp, uint8_t flags, size_t data_len, size_t data_off)
{
	mblk_t *mp;
	struct ether_header *eh;
	struct ip *ip;
	struct tcphdr *th;
	tcp_seq_t start;

	mp = str_allocb(sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr) + data_len);
	if (mp == NULL)
		return -ENOMEM;

	mp->wptr += sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr) + data_len;

	eh = (struct ether_header *)mp->rptr;
	ip = (struct ip *)(eh + 1);
	th = (struct tcphdr *)(ip + 1);

	ip->ip_v = IPVERSION;
	ip->ip_hl = sizeof(struct ip) >> 2;
	ip->ip_tos = 0;
	ip->ip_len = htons(sizeof(struct ip) + sizeof(struct tcphdr) +
	    data_len);
	ip->ip_id = htons(0); /* should be a counter */
	ip->ip_off = 0;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_TCP;
	ip->ip_sum = 0;
	ip->ip_src.s_addr = tp->laddr.sin_addr.s_addr;
	ip->ip_dst.s_addr = tp->faddr.sin_addr.s_addr;

	th->th_sport = tp->laddr.sin_port;
	th->th_dport = tp->faddr.sin_port;
	th->th_seq = htonl(tp->snd_nxt);
	th->th_ack = htonl(tp->rcv_nxt);
	th->th_off = sizeof(struct tcphdr) >> 2;
	th->th_flags = flags;
	th->th_win = htons(tp->rcv_wnd);
	th->th_x2 = 0;
	th->th_sum = 0;
	th->th_urp = 0;

	copy_data(tp, data_off, data_len, (uint8_t *)(th + 1));

	th->th_sum = tcp_checksum(ip, th, sizeof(struct tcphdr) + data_len);

	if (tp->processing_ifp != NULL)
		ip_output_intfheld(mp, tp->processing_ifp);
	else
		ip_output(mp);

	start = tp->snd_nxt;

	tp->snd_nxt += data_len;
	if (flags & (TH_SYN))
		tp->snd_nxt++;
	if (flags & (TH_FIN))
		tp->snd_nxt++;

	if (SEQ_GT(tp->snd_nxt, tp->snd_max)) {
		/* new data, not a retransmission */

		tp->snd_max = tp->snd_nxt;

		if (!tp->timing_rtt) {
			tp->timing_rtt = true;
			tp->rtstart = ke_time();
			tp->rtseq = start;
		}
	}

	/* if sending anything other than pure ACK,  ensure rexmt timer is on */
	if ((data_len != 0 || (flags & (TH_SYN | TH_FIN))) &&
	    (tp->timers[TCP_TIMER_REXMT].deadline == ABSTIME_NEVER))
		tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);

	return 0;
}

int
tcp_output(tcp_t *tp)
{
	uint8_t flags;
	int data_len, data_off;
	int swnd;
	bool can_send_more;
	int r;

	/*
	 * These are the reasons to send a packet:
	 * 1. There are SYN, RST, or FIN flags to be sent.
	 * 2. We should ACK.
	 * 3. Data is available and Nagle's algorithm allows us to send it.
	 */

send_more:
	flags = tcp_out_flags[tp->state];
	can_send_more = false;

	/*
	 * wq->q_q begins at snd_una. Therefore: snd_nxt - snd_una = the offset
	 * into the queue to take data.
	 *
	 * This would be untrue if there were a SYN sent and not yet
	 * acknowledged. But we don't send data until we're in the established
	 * state, so it's not a problem..
	 *
	 * And FIN, being the last thing sent, doesn't affect the offset.
	 */

	data_len = tcp_snd_q_count(tp);
	data_off = tp->snd_nxt - tp->snd_una;
	data_len -= data_off;
	swnd = tp->snd_wnd - data_off;

	if (data_len < 0)
		data_len = 0;

	if (swnd < 0)
		swnd = 0;

	data_len = MIN2(data_len, swnd);

	if (data_len > tp->mss) {
		data_len = tp->mss;
		can_send_more = true;
	}

	/*
	 * Nagle's algorithm.
	 * Permit sending data if window size >= MSS and available data >= MSS,
	 * or if there's no remaining unacknowledged data.
	 *
	 * Retransmission is also allowed (snd_nxt < snd_max).
	 * Also sending FIN.
	 */
	if (data_len != 0 && (flags & TH_FIN) == 0) {
		if (tp->snd_wnd < tp->mss || data_len < tp->mss) {
			if (tp->snd_una != tp->snd_max &&
			    !SEQ_LT(tp->snd_nxt, tp->snd_max)) {
				TCP_TRACE("nagle delaying send\n"
					"snd_una=%u snd_nxt=%u snd_max=%u\n"
					"snd_wnd=%u mss=%u data_len=%d\n",
				    tp->snd_una, tp->snd_nxt, tp->snd_max,
				    tp->snd_wnd, tp->mss, data_len);
				data_len = 0;
			    }
		}
	}

	/* If we're not sending everything in the queue, clear FIN. */
	if (data_off + data_len < tcp_snd_q_count(tp))
		flags &= ~TH_FIN;
	/* Don't resend FIN (unless snd_una was reset for a retransmission) */
	else if (SEQ_GT(tp->snd_nxt, tp->snd_una + tcp_snd_q_count(tp)))
		flags &= ~TH_FIN;
	else if (data_len != 0)
		flags |= TH_PUSH;

	if (tp->emit_ack) {
		tp->emit_ack = false;
		r = do_send(tp, flags, data_len, data_off);
	} else if ((flags & (TH_SYN | TH_RST | TH_FIN)) || data_len != 0) {
		r = do_send(tp, flags, data_len, data_off);
	} else {
		return 0;
	}

	if (r == 0 && can_send_more)
		goto send_more;

	return r;
}

/*
 * input processing
 */

static void
tcp_conn_con(tcp_t *tp, struct ip *ip, struct tcphdr *th, in_port_t sport)
{
	mblk_t *mp;
	struct T_conn_con *con;
	struct sockaddr_in *sin;

	kassert(tp->rq != NULL);

	kassert(tp->conn_con_m != NULL);
	mp = tp->conn_con_m;
	tp->conn_con_m = NULL;

	con = (struct T_conn_con *)mp->rptr;
	con->PRIM_type = T_CONN_CON;
	con->RES_length = sizeof(struct sockaddr_in);
	sin = (struct sockaddr_in *)&con->RES;
	sin->sin_family = AF_INET;
	sin->sin_port = sport;
	sin->sin_addr = ip->ip_src;

	str_putnext(tp->rq, mp);
}

static void
tcp_conn_ind(tcp_t *listener, tcp_t *child)
{
	mblk_t *mp;
	struct T_conn_ind *ci;
	struct sockaddr_in *sin;

	kassert(ke_mutex_held(&listener->mutex));
	kassert(ke_mutex_held(&child->mutex));
	kassert(listener->rq != NULL);
	kassert(child->conn_ind_m != NULL);
	kassert(!child->connind_sent);

	mp = child->conn_ind_m;
	child->conn_ind_m = NULL;
	child->connind_sent = true;

	mp->db->type = M_PROTO;
	ci = (struct T_conn_ind *)mp->rptr;
	ci->PRIM_type = T_CONN_IND;
	ci->SEQ_number = child->connind_seq;
	ci->SRC_length = sizeof(struct sockaddr_in);

	sin = (struct sockaddr_in *)&ci->SRC;
	sin->sin_family = AF_INET;
	sin->sin_port = child->faddr.sin_port;
	sin->sin_addr = child->faddr.sin_addr;

	mp->wptr = mp->rptr + sizeof(*ci);

	str_ingress_putq(listener->rq->stdata, mp);
}

static void
tcp_discon_ind(tcp_t *tp, int reason)
{
	mblk_t *mp;
	struct T_discon_ind *di;

	kassert(tp->discon_ind_m != NULL);
	mp = tp->discon_ind_m;
	tp->discon_ind_m = NULL;

	di = (struct T_discon_ind *)mp->rptr;
	di->PRIM_type = T_DISCON_IND;
	di->DISCON_reason = reason;
#if 0 /* old listening code */
	di->SEQ_number = (tp->listener == NULL) ? -1 : tp->conind_seq;
#endif
	di->SEQ_number = -1;

#if 0
	if (tp->listener != NULL)
		str_putnext(tp->listener->rq, mp);
	else
#endif
		str_putnext(tp->rq, mp);
}

static void
tcp_discon_ind_listener(tcp_t *listener, tcp_t *child, int reason)
{
	mblk_t *mp;
	struct T_discon_ind *di;

	kassert(ke_mutex_held(&listener->mutex));
	kassert(ke_mutex_held(&child->mutex));
	kassert(listener->rq != NULL);
	kassert(child->discon_ind_m != NULL);

	mp = child->discon_ind_m;
	child->discon_ind_m = NULL;

	mp->db->type = M_PROTO;
	di = (struct T_discon_ind *)mp->rptr;
	di->PRIM_type = T_DISCON_IND;
	di->DISCON_reason = reason;
	di->SEQ_number = child->connind_seq;
	mp->wptr = mp->rptr + sizeof(*di);

	str_ingress_putq(listener->rq->stdata, mp);
}

static void
tcp_passive_maybe_ind(tcp_t *child)
{
	tcp_t *listener;

	kassert(ke_mutex_held(&child->mutex));

	if (child->passive_listener == NULL || child->connind_sent ||
	    child->conn_ind_m == NULL || child->state < TCPS_ESTABLISHED)
		return;

	listener = child->passive_listener;
	tcp_retain(listener); /* local ref while we may relock */

	if (child > listener) {
		ke_mutex_exit(&child->mutex);
		ke_mutex_enter(&listener->mutex, "tcp_passive_maybe_ind listener");
		ke_mutex_enter(&child->mutex, "tcp_passive_maybe_ind child");
	} else {
		ke_mutex_enter(&listener->mutex, "tcp_passive_maybe_ind listener");
	}

	if (child->passive_listener == listener &&
	    child->state >= TCPS_ESTABLISHED &&
	    !child->connind_sent &&
	    !listener->closing &&
	    child->conn_ind_m != NULL)
		tcp_conn_ind(listener, child);

	ke_mutex_exit(&listener->mutex);
	tcp_release(listener);
}

static void
tcp_ordrel_ind(tcp_t *tp)
{
	mblk_t *mp;
	struct T_ordrel_ind *ord;

	kassert(tp->ordrel_ind_m != NULL);
	mp = tp->ordrel_ind_m;
	tp->ordrel_ind_m = NULL;

	ord = (struct T_ordrel_ind *)mp->rptr;
	ord->PRIM_type = T_ORDREL_IND;

	str_putnext(tp->rq, mp);
}

/* we reuse these fields for our convenience; they can be figured out again */
#define th_1stdata th_sport
#define th_datalen th_dport

static void
tcp_rput(queue_t *rq, mblk_t *mp)
{
	tcp_t *tp = rq->ptr;
	ke_mutex_enter(&tp->mutex, "tcp_rput");
	if (mp->db->type == M_DATA)
		tcp_conn_input(NULL, &tp, mp);
	else
	 	str_putnext(rq, mp); /* M_CONN_IND from remote */
	ke_mutex_exit(&tp->mutex);
}

static void
tcp_rsrv(queue_t *rq)
{
	mblk_t *mp;
	tcp_t *tp = rq->ptr;

	if (!str_canputnext(rq))
		return;

	while ((mp = str_getq(rq)) != NULL) {
		if (str_canputnext(rq)) {
			str_putnext(rq, mp);
		} else {
			str_putbq(rq, mp);
			break;
		}
	}

	if (str_canputnext(rq)) {
		size_t wnd_opening;

		kassert(TAILQ_EMPTY(&rq->msgq)); /* should be empty by now */

		if (tp->ordrel_needed) {
			tp->ordrel_needed = false;
			tcp_ordrel_ind(tp);
		}

		/* reopen the window to its maximum */
		wnd_opening = tp->rcv_wnd_max - tp->rcv_wnd;
		tp->rcv_wnd = tp->rcv_wnd_max;

		/*
		 * only send a window update if SWS-avoidance conditions
		 * met not quite RFC 9293 compliant but we aren't
		 * vulnerable to silly windows anyway; we don't reopen
		 * the window at all until we can push everything we've
		 * got upstream.
		 */
		if (wnd_opening >= tp->mss * 2 ||
		    wnd_opening >= tp->rcv_wnd_max / 2) {
			tp->emit_ack = true;
			tcp_output(tp);
		}
	}
}

static void
tcp_abort(tcp_t *tp, int err)
{
	if (tp->passive_listener != NULL) {
		tcp_t *listener = tp->passive_listener;

		tcp_retain(listener); /* local ref while we may relock */

		if (tp > listener) {
			ke_mutex_exit(&tp->mutex);
			ke_mutex_enter(&listener->mutex, "tcp_abort listener");
			ke_mutex_enter(&tp->mutex, "tcp_abort child");
		} else {
			ke_mutex_enter(&listener->mutex, "tcp_abort listener");
		}

		if (tp->passive_listener == listener) {
			if (tp->connind_sent && !listener->closing &&
			    listener->rq != NULL && tp->discon_ind_m != NULL)
				tcp_discon_ind_listener(listener, tp, err);

			tcp_passive_unlink(listener, tp);
		}

		ke_mutex_exit(&listener->mutex);
		tcp_release(listener);
	}

	if (tp->rq != NULL)
		tcp_discon_ind(tp, err);

	tcb_free_connstate(tp);

	if (tp->rq == NULL)
		tcp_release(tp);
}

static void
tcp_reply(ip_intf_t *ifpheld, mblk_t *m, struct ip *ip, struct tcphdr *th,
    tcp_seq_t seq, tcp_seq_t ack, uint8_t flags)
{
	struct in_addr tmp_addr;
	uint16_t tmp_port;
	size_t hlen = ip->ip_hl << 2;

	tmp_addr = ip->ip_src;
	ip->ip_src = ip->ip_dst;
	ip->ip_dst = tmp_addr;

	ip->ip_len = htons(hlen + sizeof(struct tcphdr));
	ip->ip_sum = 0;

	tmp_port = th->th_sport;
	th->th_sport = th->th_dport;
	th->th_dport = tmp_port;

	th->th_seq = htonl(seq);
	th->th_ack = htonl(ack);
	th->th_off = sizeof(struct tcphdr) >> 2;
	th->th_flags = flags;
	th->th_win = 0;
	th->th_sum = 0;
	th->th_urp = 0;

	m->wptr = m->rptr + sizeof(struct ether_header) + hlen +
	    sizeof(struct tcphdr);

	th->th_sum = tcp_checksum(ip, th, sizeof(struct tcphdr));

	ip_output_intfheld(m, ifpheld);
}

static void
tcp_reply_reset(ip_intf_t *ifp, mblk_t *m, struct ip *ip, struct tcphdr *th)
{
	if (th->th_flags & TH_ACK) {
		tcp_reply(ifp, m, ip, th, ntohl(th->th_ack), 0, TH_RST);
	} else {
		size_t seg_len = ntohs(ip->ip_len) - (ip->ip_hl << 2) -
		    (th->th_off << 2);
		if (th->th_flags & TH_SYN)
			seg_len++;
		if (th->th_flags & TH_FIN)
			seg_len++;
		tcp_reply(ifp, m, ip, th, ntohl(th->th_seq),
		    ntohl(th->th_ack) + seg_len, TH_RST);
	}
}

void
tcp_rtt_update(tcp_t *tp, tcp_seq_t ack_seq)
{
	kabstime_t now = ke_time();
	uint32_t r; /* RTT in milloseconds*/

	if (!tp->timing_rtt || !SEQ_GEQ(ack_seq, tp->rtseq))
		return;

	r = (uint32_t)((now - tp->rtstart) / 1000000ULL);

	TCP_TRACE("Measured RTT = %u ms for seq %u\n", r, tp->rtseq);

#define ALPHA_RTT_SHIFT	  3 /* 1/8 */
#define BETA_RTTVAR_SHIFT 2 /* 1/4 */
#define K_RTO_MULTIPLIER  4 /* 4/1 */

	if (tp->srtt == 0) {
		/*
		 * First measurement:
		 *
		 * SRTT <- R
		 * RTTVAR <- R/2
		 * RTO <- SRTT + max (G, K*RTTVAR)
		 */

		tp->srtt = r;
		tp->rttvar = r >> 1;

		TCP_TRACE("First RTT measurement - "
			"SRTT=%u ms, RTTVAR=%u ms\n",
		    tp->srtt, tp->rttvar);
	} else {
		/*
		 * Subsequent measurements:
		 *
		 * RTTVAR <- (1 - beta) * RTTVAR + beta * |SRTT - R'|
		 * SRTT <- (1 - alpha) * SRTT + alpha * R'
		 */

		uint32_t delta = (tp->srtt > r) ? (tp->srtt - r) :
						  (r - tp->srtt);

		tp->rttvar = tp->rttvar - (tp->rttvar >> BETA_RTTVAR_SHIFT) +
		    (delta >> BETA_RTTVAR_SHIFT);

		tp->srtt = tp->srtt - (tp->srtt >> ALPHA_RTT_SHIFT) +
		    (r >> ALPHA_RTT_SHIFT);

		TCP_TRACE("Updated SRTT=%u ms, RTTVAR=%u ms "
			"(measured RTT=%u ms)\n",
		    tp->srtt, tp->rttvar, r);
	}

	/* RTO <- SRTT + max (G, K*RTTVAR) [G is 1ms] */
	tp->rto = tp->srtt + MAX2(1, K_RTO_MULTIPLIER * tp->rttvar);
	tp->rto = MIN2(MAX2(tp->rto, TCP_MIN_RTO_MS), TCP_MAX_RTO_MS);

	TCP_TRACE("New RTO=%u ms\n", tp->rto);

	tp->timing_rtt = false;
	tp->n_rexmits = 0;
}

static void
tcp_snd_q_consume(tcp_t *tp, int bytes_acked)
{
	mblk_q_t *msgq;
	uint32_t *countp;
	mblk_t *m, *next;

	if (tcp_wq(tp) != NULL) {
		msgq = &tcp_wq(tp)->msgq;
		countp = &tcp_wq(tp)->count;
	} else {
		msgq = &tp->detached_snd_q;
		countp = &tp->detached_snd_q_count;
	}

	if (bytes_acked >= *countp) {
		while ((m = TAILQ_FIRST(msgq)) != NULL) {
			TAILQ_REMOVE(msgq, m, link);
			kassert(m->cont == NULL);
			str_freeb(m);
		}
		*countp = 0;
		return;
	}

	m = TAILQ_FIRST(msgq);
	while (m != NULL && bytes_acked > 0) {
		size_t size = m->wptr - m->rptr;

		kassert(m->cont == NULL);

		next = TAILQ_NEXT(m, link);

		if (bytes_acked >= size) {
			/* drop an entire mblk */
			bytes_acked -= size;
			*countp -= size;
			TAILQ_REMOVE(msgq, m, link);
			str_freeb(m);
		} else {
			/* trim the front of mblk */
			m->rptr += bytes_acked;
			*countp -= bytes_acked;
			return;
		}

		m = next;
	}
}


struct tcphdr *
mtoth(mblk_t *m)
{
	return (struct tcphdr *)(m->rptr + sizeof(struct ether_header) +
	    sizeof(struct ip));
}

bool
tcp_queue_for_reassembly(tcp_t *tp, mblk_t *m, struct tcphdr *th,
    bool *drop_mp)
{
#define SEG_LEN(th) ((th)->th_datalen + (((th)->th_flags & TH_FIN) ? 1 : 0))
	mblk_t *mq, *next, *prev;
	struct tcphdr *thq;
	bool fin_reached = false;
	tcp_seq_t seg_seq = ntohl(th->th_seq);
	tcp_seq_t seg_end = seg_seq + SEG_LEN(th);
	mblk_t *data_head = NULL, *data_tail = NULL;

	TAILQ_FOREACH (mq, &tp->reass_queue, link) {
		thq = mtoth(mq);
		if (SEQ_GT(ntohl(thq->th_seq), seg_seq))
			break;
	}

	prev = NULL;
	if (mq != NULL)
		prev = TAILQ_PREV(mq, msgb_q, link);
	else
		prev = TAILQ_LAST(&tp->reass_queue, msgb_q);

	/*
	 * Handle overlap wih preceding segments.
	 * Trim from the new segment; if completely overlapped, drop.
	 */
	if (prev != NULL) {
		struct tcphdr *thprev = mtoth(prev);
		tcp_seq_t prev_seq = ntohl(thprev->th_seq);
		tcp_seq_t prev_end = prev_seq + SEG_LEN(thprev);

		if (SEQ_GT(prev_end, seg_seq)) {
			uint32_t overlap = prev_end - seg_seq;
			uint32_t datatrim = MIN2(overlap, th->th_datalen);

			if (overlap >= SEG_LEN(th)) {
				TCP_TRACE("Segment completely overlapped by "
				    " previous segment, dropping\n");
				return false;
			}

			th->th_seq = htonl(seg_seq + datatrim);
			th->th_1stdata += datatrim;
			th->th_datalen -= datatrim;
			seg_seq += datatrim;

			/* if overlap exceeds data, then FIN gets trimmed */
			if (overlap > datatrim)
				th->th_flags &= ~TH_FIN;

			seg_end = seg_seq + SEG_LEN(th);

			TCP_TRACE("Trimmed %u bytes%s from beginning of new "
				  "segment\n",
			    datatrim, (overlap > datatrim) ? " and FIN" : "");
		}
	}

	/* this mblk is being kept - we now own it */
	*drop_mp = false;

	if (mq != NULL)
		TAILQ_INSERT_BEFORE(mq, m, link);
	else
		TAILQ_INSERT_TAIL(&tp->reass_queue, m, link);


	/*
	 * Handle overlap wih succeeding segments.
	 * Trim from the succeeding segment; if completely overlapped, drop.
	 */
	mq = TAILQ_NEXT(m, link);
	while (mq != NULL) {
		tcp_seq_t succ_seq;

		thq = mtoth(mq);
		succ_seq = ntohl(thq->th_seq);

		if (SEQ_GT(seg_end, succ_seq)) {
			uint32_t overlap = seg_end - succ_seq;

			if (overlap >= SEG_LEN(thq)) {
				TCP_TRACE("Succeeding segment completely "
				    "overlapped, removing\n");
				next = TAILQ_NEXT(mq, link);
				TAILQ_REMOVE(&tp->reass_queue, mq, link);
				str_freemsg(mq);
				mq = next;
				continue;
			} else {
				uint32_t datatrim = MIN2(overlap,
				    thq->th_datalen);

				thq->th_seq = htonl(succ_seq + datatrim);
				thq->th_1stdata += datatrim;
				thq->th_datalen -= datatrim;

				if (overlap > datatrim)
					thq->th_flags &= ~TH_FIN;

				TCP_TRACE("Trimmed %u bytes%s from beginning "
					  "of succeeding segment\n",
				    overlap,
				    (overlap > datatrim) ? " and FIN" : "");
			}
		} else {
			/* no overlap */
			break;
		}

		mq = TAILQ_NEXT(mq, link);
	}

	mq = TAILQ_FIRST(&tp->reass_queue);
	while (mq != NULL) {
		thq = mtoth(mq);
		uint32_t cur_seq = ntohl(thq->th_seq);

		if (cur_seq != tp->rcv_nxt)
			break;

		TCP_TRACE("Processing contiguous segment, seq=%u, len=%u\n",
		    cur_seq, thq->th_datalen);

		tp->rcv_nxt += thq->th_datalen;

		if (thq->th_flags & TH_FIN) {
			TCP_TRACE("FIN segment processed\n");
			tp->rcv_nxt++;
			fin_reached = true;
		}

		next = TAILQ_NEXT(mq, link);
		TAILQ_REMOVE(&tp->reass_queue, mq, link);

		/* strip headers ready for passing upstream */
		if (thq->th_datalen > 0) {
			mq->rptr = (char *)thq + thq->th_1stdata;
			mq->wptr = mq->rptr + thq->th_datalen;
			mq->db->type = M_DATA;
			mq->cont = NULL;

			if (data_head == NULL) {
				data_head = data_tail = mq;
			} else {
				data_tail->cont = mq;
				data_tail = mq;
			}
		} else {
			str_freemsg(mq);
		}

		mq = next;
	}

	if (data_head != NULL) {
		/*
		 * We manage rcv_wnd like this: it stays constant until we can't
		 * putnext() any further data and have to queue it; then, we
		 * reduce rcv_wnd by as much as we receive. this continues until
		 * we are free to putnext() again, at which point we reopen the
		 * window to its full extent.
		 */

		if (tp->rq != NULL && str_canputnext(tp->rq)) {
			if (!TAILQ_EMPTY(&tp->rq->msgq)) {
				kassert(tp->rq->enabled);
				tp->rcv_wnd -= str_msgsize(data_head);
				str_putq(tp->rq, data_head);
				tcp_rsrv(tp->rq);
				tp->rq->enabled = false;
			} else {
				str_putnext(tp->rq, data_head);
			}
		} else if (tp->rq == NULL) {
			TAILQ_INSERT_TAIL(&tp->detached_rcv_q, data_head, link);
			tp->detached_rcv_q_count += str_msgsize(data_head);
			tp->rcv_wnd -= str_msgsize(data_head);
		} else {
			str_putq(tp->rq, data_head);
			tp->rcv_wnd -= str_msgsize(data_head);
		}
	}

	return fin_reached;
}

static tcp_t *
tcp_passive_open(tcp_t *listener, struct ip *ip, struct tcphdr *th,
    in_port_t sport, in_port_t dport)
{
	tcp_t *child;
	struct sockaddr_in faddr;
	struct ip_route_result rt;
	int r;

	rt = ip_route_lookup(ip->ip_src);
	if (rt.intf == NULL)
		return NULL;

	child = tcp_new(NULL);
	if (child == NULL) {
		ip_intf_release(rt.intf);
		return NULL;
	}

	ke_mutex_enter(&child->mutex, "tcp_passive_open child");

	child->mss = rt.intf->mtu - sizeof(struct ip) - sizeof(struct tcphdr);
	ip_intf_release(rt.intf);

	child->laddr.sin_family = AF_INET;
	child->laddr.sin_port = dport;
	child->laddr.sin_addr = ip->ip_dst;

	faddr.sin_family = AF_INET;
	faddr.sin_port = sport;
	faddr.sin_addr = ip->ip_src;

	r = tcp_setup_connection(child, &faddr);
	if (r != 0)
		goto fail;

	child->conn_ind_m = str_allocb(sizeof(struct T_conn_ind));
	if (child->conn_ind_m == NULL)
		goto fail;

	child->conn_ind_m->db->type = M_PROTO;
	child->conn_ind_m->wptr += sizeof(struct T_conn_ind);

	r = tcp_conn_table_insert(child);
	if (r != 0)
		goto fail;

	child->irs = ntohl(th->th_seq);
	child->rcv_nxt = child->irs + 1;
	child->snd_wnd = ntohs(th->th_win);
	child->snd_wl1 = ntohl(th->th_seq);
	child->snd_wl2 = 0;

	child->passive_listener = listener;
	tcp_retain(listener); /* child ref on listener */

	child->connind_seq = listener->next_connind_seq++;
	LIST_INSERT_HEAD(&listener->conninds, child, conninds_entry);
	tcp_retain(child);    /* listener list ref on child */

	tcp_change_state(child, TCPS_SYN_RECEIVED);
	return child;

fail:
	ke_mutex_exit(&child->mutex);
	tcb_free_connstate(child);
	tcp_release(child);
	return NULL;
}

/*
 * entered with tp->mutex held
 * if detached, ifp is the interface whose processing context we're in (for
 * passing to IP)
 * if not detached, then entered from stream processing context (so stream mutex
 * also held).
 */
void
tcp_conn_input(ip_intf_t *ifp, tcp_t **tpp, mblk_t *mp)
{
	struct ip *ip = (struct ip *)(mp->rptr + sizeof(struct ether_header));
	uint16_t hlen = ip->ip_hl << 2;
	uint16_t tcp_len = ntohs(ip->ip_len) - hlen;
	struct tcphdr *th = (struct tcphdr *)((char *)ip + hlen);
	bool got_fin = false;
	uint16_t sport, dport;
	bool dropmp = true;
	tcp_t *tp = *tpp;

	tp->processing_ifp = ifp;

	sport = th->th_sport;
	dport = th->th_dport;
#define TCP_REPLY_RESET(ifp, mp, ip, th) \
do { \
	th->th_sport = sport; \
	th->th_dport = dport; \
	tcp_reply_reset(ifp, mp, ip, th); \
	(mp) = NULL; \
} while (0)

	th->th_1stdata = (th->th_off << 2);
	th->th_datalen = tcp_len - th->th_1stdata;

	/* 3.10.7: SEGMENT ARRIVES */
	switch (tp->state) {
	case TCPS_CLOSED:
		TCP_REPLY_RESET(ifp, mp, ip, th);
		dropmp = false;
		goto finish;

	case TCPS_LISTEN:
		/* 3.10.7.2: If the state is LISTEN, then... */

		if (th->th_flags & TH_RST)
			goto finish;

		if (th->th_flags & TH_ACK) {
			TCP_REPLY_RESET(ifp, mp, ip, th);
			dropmp = false;
			goto finish;
		}

		if (th->th_flags & TH_SYN) {
			tcp_t *listener = tp;
			tcp_t *child;

			if (listener->closing)
				goto finish;

			child = tcp_passive_open(listener, ip, th, sport,
			    dport);
			if (child == NULL)
				goto finish;

			/*
			 * Henceforth processing is now on the passive child, no
			 * longer the listener.
			 * child->mutex is held on return from
			 * tcp_passive_open().
			 */
			listener->processing_ifp = NULL;
			ke_mutex_exit(&listener->mutex);

			tp = child;
			tp->processing_ifp = ifp;
			*tpp = tp;

			/*
			 * We have accepted the SYN into the child's
			 * IRS/RCV.NXT. Consume the SYN in the incoming segment
			 * and continue with normal text / FIN processing on the
			 * child.
			 */
			th->th_seq = htonl(ntohl(th->th_seq) + 1);

			if (th->th_datalen > 0 &&
			    th->th_datalen > tp->rcv_wnd) {
				th->th_flags &= ~TH_FIN;
				th->th_datalen = MIN2(th->th_datalen,
				    tp->rcv_wnd);
			}

			goto step_6;
		}

		/* no valid case */
		goto finish;

	case TCPS_SYN_SENT:
		/*
		 * 3.10.7.3: If the state is SYN-SENT, then
		 * First, check the ACK bit:
		 */
		if (th->th_flags & TH_ACK) {
			if (SEQ_LEQ(ntohl(th->th_ack), tp->iss) ||
			    SEQ_GT(ntohl(th->th_ack), tp->snd_max)) {
				/*
				 * If SEG.ACK =< ISS or SEG.ACK > SND.NXT, send
				 * a reset (unless the RST bit is set, if so
				 * drop the segment and return)
				 */
				TCP_REPLY_RESET(ifp, mp, ip, th);
				dropmp = false;
				goto finish;
			}
		}

		if (th->th_flags & TH_RST) {
			if (th->th_flags & TH_ACK) {
				/*
				 * If the ACK was acceptable, then signal to the
				 * user "error: connection reset", drop the
				 * segment, enter CLOSED state, delete TCB, and
				 * return.
				 */
				tcp_abort(tp, ECONNRESET);
				goto finish;
			} else {
				/* Otherwise, drop the segment and return. */
				goto finish;
			}
		}

		if (th->th_flags & TH_SYN) {
			/*
			 * If the SYN bit is on and the security/compartment is
			 * acceptable, then RCV.NXT is set to SEG.SEQ+1, IRS is
			 * set to SEG.SEQ...
			 */

			tp->irs = ntohl(th->th_seq);
			tp->rcv_nxt = tp->irs + 1;

			/* ...SND.UNA should be advanced to equal SEG.ACK (if
			 * there is an ACK), and any segments on the
			 * retransmission queue that are thereby acknowledged
			 * should be removed.
			 *
			 * [we don't send any data with the SYN so no need.]
			 */
			if (th->th_flags & TH_ACK)
				tp->snd_una = ntohl(th->th_ack);

			if (SEQ_GT(tp->snd_una, tp->iss)) {
				/*
				 * If SND.UNA > ISS (our SYN has been ACKed),
				 * change the connection state to ESTABLISHED,
				 * form an ACK segment and send it.
				 *
				 * [this will happen next tcp_output()]
				 */
				tcp_change_state(tp, TCPS_ESTABLISHED);
#if 0
				kassert(tp->listener == NULL);
#endif
				tcp_conn_con(tp, ip, th, sport);
				tp->emit_ack = true;
			} else {
				/*
				 * Otherwise, enter SYN-RECEIVED, form a SYN,ACK
				 * segment, and send it. Set the variables:
				 */
				tcp_change_state(tp, TCPS_SYN_RECEIVED);
				tp->emit_ack = true;
			}

			tp->snd_wnd = ntohs(th->th_win);
			tp->snd_wl1 = ntohl(th->th_seq);
			tp->snd_wl2 = ntohl(th->th_ack);

			/* Maybe update RTT based on the SYN we sent. */
			tcp_cancel_timer(tp, TCP_TIMER_REXMT);
			tcp_rtt_update(tp, ntohl(th->th_ack));

			/* skip the SYN space & trim excess data if any */
			th->th_seq = htonl(ntohl(th->th_seq) + 1);

			if (th->th_datalen > 0 &&
			    th->th_datalen > tp->rcv_wnd) {
				th->th_flags &= ~TH_FIN;
				th->th_datalen = MIN2(th->th_datalen,
				    tp->rcv_wnd);
			}

			goto step_6;
		}

		/*
		 * Fifth, if neither of the SYN or RST bits is set, then drop
		 * the segment and return.
		 */
		if (!(th->th_flags & (TH_SYN | TH_RST)))
			goto finish;

	default:
		break;
	}

	/* First, check sequence number: */

	if (tp->rcv_wnd == 0) {
		/*
		 * If the RCV.WND is zero, no segments will be acceptable, but
		 * special allowance should be made to accept valid ACKs, URGs,
		 * and RSTs.
		 * [SEG.SEQ = RCV.NXT - trim any data, FIN, PUSH]
		 */

		if (ntohl(th->th_seq) != tp->rcv_nxt) {
			/*
			 * If an incoming segment is not acceptable, an
			 * acknowledgment should be sent in reply (unless the
			 * RST bit is set, if so drop the segment and return)
			 */
			tp->emit_ack = true;
			tcp_output(tp);
			goto finish;
		} else {
			tcp_len = sizeof(struct tcphdr);
			th->th_datalen = 0;
			th->th_flags &= ~(TH_FIN | TH_PUSH);
		}
	} else {
		int32_t trim_lead, trim_tail;

		trim_lead = tp->rcv_nxt - ntohl(th->th_seq);
		if (trim_lead > 0) {
			if (th->th_flags & TH_SYN) {
				/* trim logically leading syn */
				trim_lead--;
				th->th_seq = htonl(ntohl(th->th_seq) + 1);
				th->th_flags &= ~TH_SYN;

				if (th->th_urp != 0)
					th->th_urp = htons(ntohs(th->th_urp) -
					    1);
				else
					th->th_flags &= ~TH_URG;
			}

			if (trim_lead > th->th_datalen ||
			    ((trim_lead == th->th_datalen) &&
				(th->th_flags & TH_FIN))) {
				tp->emit_ack = true;
				tcp_output(tp);
				goto finish;
			}

			th->th_seq = htonl(ntohl(th->th_seq) + trim_lead);
			th->th_1stdata += trim_lead;
			th->th_datalen -= trim_lead;

			if (ntohs(th->th_urp) > trim_lead) {
				th->th_urp = htons(
				    ntohs(th->th_urp) - trim_lead);
			} else {
				th->th_flags &= ~TH_URG;
				th->th_urp = 0;
			}
		}

		trim_tail = (ntohl(th->th_seq) + th->th_datalen) -
		    (tp->rcv_nxt + tp->rcv_wnd);
		if (trim_tail > 0) {
			if (trim_tail >= th->th_datalen) {
				tp->emit_ack = true;
				tcp_output(tp);
				goto finish;
			}

			th->th_datalen -= trim_tail;
			th->th_flags &= ~(TH_PUSH | TH_FIN);
		}
	}

	/* Second, check the RST bit: */
	if (th->th_flags & TH_RST) {
		/*
		 * If the RST bit is set and the sequence number is outside the
		 * current receive window, silently drop the segment.
		 */
		if (SEQ_LT(ntohl(th->th_seq), tp->rcv_nxt) ||
		    SEQ_GT(ntohl(th->th_seq), tp->rcv_nxt + tp->rcv_wnd)) {
			goto finish;
		} else if (ntohl(th->th_seq) == tp->rcv_nxt) {
			/*
			 * If the RST bit is set and the sequence number exactly
			 * matches the next expected sequence number (RCV.NXT),
			 * then TCP endpoints MUST reset the connection in the
			 * manner prescribed below according to the connection
			 * state.
			 */
			switch (tp->state) {
			case TCPS_SYN_RECEIVED:
				tcp_abort(tp, ECONNRESET);
				goto finish;

			case TCPS_ESTABLISHED:
			case TCPS_FIN_WAIT_1:
			case TCPS_FIN_WAIT_2:
			case TCPS_CLOSE_WAIT:
			/*
			 * RFC specifies different handling for the below; all
			 * these reflect detached TCBs, and tcp_abort() does the
			 * needful in that case.
			 */
			case TCPS_CLOSING:
			case TCPS_LAST_ACK:
			case TCPS_TIME_WAIT:
				tcp_abort(tp, ECONNRESET);
				goto finish;

			default:
				kunreachable();
			}
		} else if (SEQ_LEQ(ntohl(th->th_seq),
			       tp->rcv_nxt + tp->rcv_wnd)) {
			/*
			 * If the RST bit is set and the sequence number does
			 * not exactly match the next expected sequence value,
			 * yet is within the current receive window, TCP
			 * endpoints MUST send an acknowledgment (challenge
			 * ACK):
			 *
			 * <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
			 *
			 * After sending the challenge ACK, TCP endpoints MUST
			 * drop the unacceptable segment and stop processing the
			 * incoming packet further. Note that RFC 5961 and
			 * Errata ID 4772 [99] contain additional considerations
			 * for ACK throttling in an implementation.
			 */
			th->th_sport = sport;
			th->th_dport = dport;
			tcp_reply(ifp, mp, ip, th, tp->snd_nxt, tp->rcv_nxt,
			    TH_ACK);
			dropmp = false;
			goto finish;
		}
	}

	/* Fourth, check the SYN bit: */
	if (th->th_flags & TH_SYN) {
		/*
		 * If the SYN is in the window it is an error: send a reset, any
		 * outstanding RECEIVEs and SEND should receive "reset"
		 * responses, all segment queues should be flushed, the user
		 * should also receive an unsolicited general "connection reset"
		 * signal, enter the CLOSED state, delete the TCB, and return.
		 */

		tcp_abort(tp, ECONNRESET);
		TCP_REPLY_RESET(ifp, mp, ip, th);
		dropmp = false;
		goto finish;
	}

	/* Fifth, check the ACK field: */
	if (!(th->th_flags & TH_ACK)) {
		/* if the ACK bit is off, drop the segment and return */
		if (tp->emit_ack)
			tcp_output(tp);
		goto finish;
	}

	switch (tp->state) {
	case TCPS_SYN_RECEIVED:
		/*
		 * If SND.UNA < SEG.ACK =< SND.NXT, then enter ESTABLISHED state
		 * and continue processing with the variables below set to:
		 */
		if (SEQ_LT(tp->snd_una, ntohl(th->th_ack)) &&
		    SEQ_LEQ(ntohl(th->th_ack), tp->snd_max)) {
			/* advance for SYN bit */
			tp->snd_una++;

			if (SEQ_GT(tp->snd_una, tp->snd_nxt))
#if 0 /* don't remember adding this - what is it good for? */
				tp->snd_max = tp->snd_nxt;
#else
				kfatal("how?\n");
#endif

			tcp_change_state(tp, TCPS_ESTABLISHED);

			if (tp->passive_listener == NULL)
				tcp_conn_con(tp, ip, th, sport);
			else
				tcp_passive_maybe_ind(tp);

			tp->snd_wnd = ntohs(th->th_win);
			tp->snd_wl1 = ntohl(th->th_seq);
			tp->snd_wl2 = ntohl(th->th_ack);

			goto acceptable_ack;
		} else {
			TCP_REPLY_RESET(ifp, mp, ip, th);
			dropmp = false;
			goto finish;
		}

	case TCPS_ESTABLISHED:
	case TCPS_FIN_WAIT_1:
	case TCPS_FIN_WAIT_2:
	case TCPS_CLOSE_WAIT:
	case TCPS_CLOSING:
	case TCPS_LAST_ACK:
	case TCPS_TIME_WAIT: {
		int nseq_acked = 0;
		bool our_fin_acked = false;

		if (SEQ_LEQ(ntohl(th->th_ack), tp->snd_una)) {
			/*
			 * If the ACK is a duplicate (SEG.ACK =< SND.UNA), it
			 * can be ignored.
			 */
		} else if (SEQ_GT(ntohl(th->th_ack), tp->snd_max)) {
			/*
			 * If the ACK acks something not yet sent (SEG.ACK >
			 * SND.NXT), then send an ACK, drop the segment, and
			 * return.
			 * [snd_max in our case.]
			 */
			tp->emit_ack = true;
			tcp_output(tp);
			goto finish;
		} else {
		acceptable_ack:
			nseq_acked = ntohl(th->th_ack) - tp->snd_una;

			tp->snd_una = ntohl(th->th_ack);

			/* if everything is ACKed, stop rexmt timer */
			if (tp->snd_una == tp->snd_max)
				tcp_cancel_timer(tp, TCP_TIMER_REXMT);

			if (SEQ_LT(tp->snd_nxt, tp->snd_una))
				tp->snd_nxt = tp->snd_una;

			/* Maybe update RTT. */
			tcp_rtt_update(tp, ntohl(th->th_ack));

			/*
			 * If SND.UNA =< SEG.ACK =< SND.NXT, the send window
			 * should be updated.
			 */

			if (nseq_acked > tcp_snd_q_count(tp)) {
				tp->snd_wnd -= tcp_snd_q_count(tp);
				tcp_snd_q_consume(tp, tcp_snd_q_count(tp));
				our_fin_acked = true;
			} else {
#if 0 /* why did I add this? */
				tp->snd_wnd -= nseq_acked;
#endif
				tcp_snd_q_consume(tp, nseq_acked);
			}
		}

		/*
		 * If (SND.WL1 < SEG.SEQ or (SND.WL1 = SEG.SEQ and
		 * SND.WL2 =< SEG.ACK)), set SND.WND <- SEG.WND, set SND.WL1 <-
		 * SEG.SEQ, and set SND.WL2 <- SEG.ACK.
		 */
		if (SEQ_LT(tp->snd_wl1, ntohl(th->th_seq)) ||
		    (tp->snd_wl1 == ntohl(th->th_seq) &&
			SEQ_LEQ(tp->snd_wl2, ntohl(th->th_ack)))) {
			TCP_TRACE("snd_wnd now %d, wl1 now %u, wl2 now %u\n",
			    ntohs(th->th_win), ntohl(th->th_seq),
			    ntohl(th->th_ack));
			tp->snd_wnd = ntohs(th->th_win);
			tp->snd_wl1 = ntohl(th->th_seq);
			tp->snd_wl2 = ntohl(th->th_ack);
			if (tp->snd_wnd != 0)
				tcp_cancel_timer(tp, TCP_TIMER_PERSIST);
		}

		switch (tp->state) {
		case TCPS_FIN_WAIT_1:
			if (our_fin_acked) {
				tcp_change_state(tp, TCPS_FIN_WAIT_2);
				if (tp->rq == NULL || tp->shutdown_rd) {
					/*
					 * Do the nonstandard BSD behaviour
					 * here, whereby if there's no way to
					 * further read from this connection
					 * (due to shutdown SHUT_RD or the TCB
					 * being detached), we set a 2MSL timer
					 * and give up if we don't see FIN from
					 * the remote in that time.
					 */

					tcp_set_timer(tp, TCP_TIMER_2MSL,
					    TCP_2MSL_MS);
				}
			}
			break;

		case TCPS_CLOSING:
			/*
			 * If the ACK acknowledges our FIN, then enter the
			 * TIME-WAIT state; otherwise, ignore the segment
			 */
			if (our_fin_acked) {
				tcp_change_state(tp, TCPS_TIME_WAIT);
				tcp_cancel_all_timers(tp);
				tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			} else {
				goto finish;
			}
			break;

		case TCPS_LAST_ACK:
			/*
			 * The only thing that can arrive in this state is an
			 * acknowledgment of our FIN. If our FIN is now
			 * acknowledged, delete the TCB, enter the CLOSED state,
			 * and return.
			 */
			if (our_fin_acked) {
				tcb_free_connstate(tp);
				tcp_release(tp);
				goto finish;
			}
			break;

		case TCPS_TIME_WAIT:
			/*
			 * The only thing that can arrive in this state is a
			 * retransmission of the remote FIN. Acknowledge it and
			 * restart the 2MSL timer.
			 */
			tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			tp->emit_ack = true;
			tcp_output(tp);
			goto finish;

		default:
			break;
		}
		break;

	default:
		kunreachable();
	}
	}

step_6:
	/* Sixth, check the URG bit: */
	if (th->th_flags & TH_URG)
		TCP_TRACE("TH_URG currently ignored!!");

	/* Seventh, process the segment text: */
	if (th->th_datalen > 0 || (th->th_flags & (TH_FIN))) {
		TCP_TRACE("Processing data segment, seq=%d (rcv_nxt=%d)\n",
		    ntohl(th->th_seq), tp->rcv_nxt);
		got_fin = tcp_queue_for_reassembly(tp, mp, th, &dropmp);
		tp->emit_ack = true;
	}

	/* Eighth, check the FIN bit: */
	if (got_fin) {
		if (tp->state <= TCPS_SYN_SENT) {
			/*
			 * Do not process the FIN if the state is CLOSED,
			 * LISTEN, or SYN-SENT since the SEG.SEQ cannot be
			 * validated; drop the segment and return.
			 */
			goto finish;
		}

		/*
		 * If the FIN bit is set, signal the user "connection
		 * closing" and return any pending RECEIVEs with same
		 * message, advance RCV.NXT over the FIN, and send an
		 * acknowledgment for the FIN. Note that FIN implies
		 * PUSH for any segment text not yet delivered to the
		 * user.
		 *
		 * [the RCV.NXT advancement was done in
		 * tcp_queue_for_reassembly]
		 */

		switch (tp->state) {
		case TCPS_SYN_RECEIVED:
		case TCPS_ESTABLISHED:
			tcp_change_state(tp, TCPS_CLOSE_WAIT);
			tp->ordrel_needed = true;
			break;

		case TCPS_FIN_WAIT_1:
			/*
			 * If our FIN has been ACKed (perhaps in this segment),
			 * then enter TIME-WAIT, start the time-wait timer, turn
			 * off the other timers; otherwise, enter the CLOSING
			 * state.
			 *
			 * [if our FIN was acked, it would have been processed
			 * above in the ACK handling and we'd be in FIN-WAIT-2.
			 * Therefore, it must be unACKed so we go to CLOSING.]
			 */
			tcp_change_state(tp, TCPS_CLOSING);
			tp->ordrel_needed = true;
			break;

		case TCPS_FIN_WAIT_2:
			tcp_change_state(tp, TCPS_TIME_WAIT);
			tp->ordrel_needed = true;
			tcp_cancel_all_timers(tp);
			tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			break;

		case TCPS_CLOSE_WAIT:
		case TCPS_CLOSING:
		case TCPS_LAST_ACK:
			/* remain in these states */
			break;

		case TCPS_TIME_WAIT:
			/*
			 * Remain in the TIME-WAIT state. Restart the 2 MSL
			 * time-wait timeout.
			 */
			tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			break;

		default:
			kunreachable();
		}

		if (tp->ordrel_needed && tp->rq != NULL &&
		    TAILQ_EMPTY(&tp->rq->msgq))
			tcp_ordrel_ind(tp);
	}

	tcp_output(tp);

finish:
	tp->processing_ifp = NULL;

	if (dropmp)
		str_freemsg(mp);
}

void
tcp_input(ip_intf_t *ifp, mblk_t *mp)
{
	struct ip *ip = (struct ip *)(mp->rptr + sizeof(struct ether_header));
	uint16_t hlen = ip->ip_hl << 2;
	uint16_t tcp_len;
	struct tcphdr *th = (struct tcphdr *)((char *)ip + hlen);
	tcp_t *tp, *tp2;

	tcp_len = ntohs(ip->ip_len) - hlen;
	if (tcp_len < sizeof(struct tcphdr)) {
		TCP_TRACE("Header too small\n");
		str_freemsg(mp);
		return;
	}

	/* prettyprint key facts of packet */
	TCP_TRACE(" -- Received TCP packet: src=" FMT_IP4 ":%u dst=" FMT_IP4 ":%u "
	    "seq=%u ack=%u flags=%s%s%s%s%s%s\n",
	    ARG_IP4(ip->ip_src.s_addr), ntohs(th->th_sport),
	    ARG_IP4(ip->ip_dst.s_addr), ntohs(th->th_dport),
	    ntohl(th->th_seq), ntohl(th->th_ack),
	    (th->th_flags & TH_FIN) ? "FIN " : "",
	    (th->th_flags & TH_SYN) ? "SYN " : "",
	    (th->th_flags & TH_RST) ? "RST " : "",
	    (th->th_flags & TH_PUSH) ? "PSH " : "",
	    (th->th_flags & TH_ACK) ? "ACK " : "",
	    (th->th_flags & TH_URG) ? "URG " : "");

	tp = tcp_conn_lookup(ip->ip_src, th->th_sport, ip->ip_dst,
	    th->th_dport);

	if (tp == NULL) {
		TCP_TRACE("No matching connection found\n");
		tcp_reply_reset(ifp, mp, ip, th);
		return;
	}

	tp2 = tp;

	ke_mutex_enter(&tp->mutex, "tcp_input");
	if (tp->rq != NULL)
	 	str_ingress_putq(tp->rq->stdata, mp);
	else
		tcp_conn_input(ifp, &tp2, mp);
	ke_mutex_exit(&tp2->mutex);

	tcp_release(tp);
}

/*
 * timers
 */
void
tcp_set_timer(tcp_t *tp, enum tcp_timer_type type, uint32_t timeout_ms)
{
	kabstime_t deadline;

	deadline = ke_time() + (kabstime_t)timeout_ms * NS_PER_MS;
	tcp_cancel_timer(tp, type);
	tp->timers[type].deadline = deadline;
	ke_callout_set(&tp->timers[type].callout, deadline);
}

void
tcp_cancel_timer(tcp_t *tp, enum tcp_timer_type type)
{
	tp->timers[type].deadline = ABSTIME_NEVER;
	ke_callout_stop(&tp->timers[type].callout);
}

void
tcp_cancel_all_timers(tcp_t *tp)
{
	for (int i = 0; i < TCP_TIMER_MAX; i++)
		tcp_cancel_timer(tp, i);
}

static void
tcp_timer_dpchandler(void *arg, void *timer_arg)
{
	tcp_t *tp = arg;
	struct tcp_timer *timer = timer_arg;
	int timer_type = timer - tp->timers;
	ipl_t ipl;

	if (!tcp_tryretain(tp))
		return;

	atomic_fetch_or(&tp->pending_timers, (1 << timer_type));

	ipl = ke_spinlock_enter(&tp->detach_lock);

	if (tp->rq != NULL) {
		/* current attached. Wsrv routine handles pending timers. */
		str_qenable(tcp_wq(tp));
		str_kick(tp->rq->stdata);
		ke_spinlock_exit(&tp->detach_lock, ipl);
		tcp_release(tp); /* Release our local DPC ref */
	} else {
		/* currently detached. Put to a TCP worker thread. */
		ke_spinlock_exit(&tp->detach_lock, ipl);

		ipl = ke_spinlock_enter(&tcp_detached_lock);
		if (!tp->on_detached_work_q) {
			tp->on_detached_work_q = true;
			TAILQ_INSERT_TAIL(&tcp_detached_work_q, tp, detached_link);
			ke_event_set_signalled(&tcp_detached_event, true);
		}
		ke_spinlock_exit(&tcp_detached_lock, ipl);
		/* no tcp_release(); the reference is the worker thread's now */
	}
}

static void
tcp_rexmt_timer(tcp_t *tp)
{
	TCP_TRACE("TCP retransmit timer expired\n");

	if (tp->timers[TCP_TIMER_REXMT].deadline > ke_time())
		return;

	if (tp->n_rexmits++ == TCP_MAXRETRIES) {
		TCP_TRACE("Too many rexmit attempts, giving up\n");
		tcp_abort(tp, ETIMEDOUT);
		return;
	}

	/*
	 * Retransmit timer expired - stop timing RTT, backoff RTO, and let the
	 * next sequence to be sent be the first unacknowledged one.
	 */

	tp->timing_rtt = false;
	tp->rto = MIN2(tp->rto * 2, TCP_MAX_RTO_MS);
	tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);

	tp->snd_nxt = tp->snd_una;
	tcp_output(tp);
}

static void
tcp_persist_timer(tcp_t *)
{
	TCP_TRACE("TODO: TCP persist timer expired\n");
}

static void
tcp_keepalive_timer(tcp_t *)
{
	TCP_TRACE("TODO: TCP keepalive timer expire\n");
}

static void
tcp_2msl_timer(tcp_t *tp)
{
	TCP_TRACE("TCP 2MSL timer expired\n");

	if (tp->timers[TCP_TIMER_2MSL].deadline > ke_time())
		return;

	tcb_free_connstate(tp);
	tcp_release(tp);
}

static void
tcp_detached_worker(void *arg)
{
	for (;;) {
		ke_wait1(&tcp_detached_event, "tcp detached wait", false,
		    ABSTIME_FOREVER);
		ke_event_set_signalled(&tcp_detached_event, false);

		for (;;) {
			tcp_t *tp;
			uint32_t pending;
			ipl_t ipl;

			ipl = ke_spinlock_enter(&tcp_detached_lock);
			tp = TAILQ_FIRST(&tcp_detached_work_q);
			if (tp == NULL) {
				ke_spinlock_exit(&tcp_detached_lock, ipl);
				break;
			}
			TAILQ_REMOVE(&tcp_detached_work_q, tp, detached_link);
			tp->on_detached_work_q = false;
			ke_spinlock_exit(&tcp_detached_lock, ipl);

			pending = atomic_exchange(&tp->pending_timers, 0);

			ke_mutex_enter(&tp->mutex, "tcp_detached_timers");
			if (tp->rq != NULL) {
				/* reattached while pending. wsrv will handle */
				ke_mutex_exit(&tp->mutex);
				tcp_release(tp);
				continue;
			}

			if (pending & (1 << TCP_TIMER_REXMT))
				tcp_rexmt_timer(tp);
			if (pending & (1 << TCP_TIMER_2MSL))
				tcp_2msl_timer(tp);
			if (pending & (1 << TCP_TIMER_PERSIST))
				tcp_persist_timer(tp);
			if (pending & (1 << TCP_TIMER_KEEPALIVE))
				tcp_keepalive_timer(tp);
			ke_mutex_exit(&tp->mutex);

			tcp_release(tp); /* tp was retained in dpc handler */
		}
	}
}

void
tcp_init(void)
{
	static thread_t *tcp_timer_thread;

	TAILQ_INIT(&tcp_detached_work_q);
	ke_event_init(&tcp_detached_event, false);
	tcp_timer_thread = proc_new_system_thread(tcp_detached_worker, NULL);
	ke_thread_resume(&tcp_timer_thread->kthread, false);
}
