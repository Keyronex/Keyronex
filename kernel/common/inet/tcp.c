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
 * Locking
 * -------
 *
 * Each TCB has a spinlock (tcp_t::lock) that protects all its state.
 * The connection table is protected by tcp_conntab_lock.
 * Lock ordering when acquiring multiple TCB locks: by ascending pointer value.
 *
 * DPC context (tcp_ipv4_input, timer DPCs) runs at IPL_DISP.  The TCB
 * spinlock raises to IPL_DISP, so it may be held by both thread-level and
 * DPC-level code without issue.
 *
 * Upstream delivery
 * -----------------
 *
 * str_putnext() cannot be called while the TCB spinlock is held (it would
 * try to acquire a sleeping stream mutex).  Instead, received data is stored
 * in tcp_t::rcv_q and pending control-message flags are set, then the read
 * queue is qenable()'d.  tcp_rsrv() runs at thread level and flushes
 * everything upstream.
 *
 * str_ingress_putq() is safe to call under the spinlock (it uses its own
 * internal spinlock) and is used for T_CONN_IND / T_DISCON_IND to a
 * listener.
 *
 * Existence guarantees
 * --------------------
 *
 * The connection table holds one reference per entry.  RCU (via
 * ke_rcu_call) defers the actual free until all in-flight DPCs that may
 * have obtained a pointer to the TCB are done.
 */

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/k_rcu.h>
#include <sys/libkern.h>
#include <sys/queue.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/tihdr.h>

#include <netinet/in.h>
#include <netinet/ip.h>

#include <inet/ip.h>
#include <inet/tcphdr.h>
#include <inet/util.h>

#define TCP_WINDOW	 (536 * 8)
#define TCP_MSS		 536

#define TCP_MAXRETRIES	 5

#define TCP_INIT_RTO_MS	 1000
#define TCP_MAX_RTO_MS	 60000	/* 60 seconds */
#define TCP_MIN_RTO_MS	 200	/* 200 milliseconds (deliberately not RFC) */
#define TCP_2MSL_MS	 60000	/* 60 seconds */
#define TCP_KEEPALIVE_MS 7200000 /* 2 hours */

/* state machine & types*/

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
	TCPS_LAST_ACK,		/* receiver closing; sent FIN */
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
	kspinlock_t	lock;
	atomic_uint	refcnt;

	queue_t		*rq;		/* stream read queue, if not detached */
	mblk_q_t	rcv_q;
	uint32_t	rcv_q_count;

	/* send queue when detached */
	mblk_q_t	detached_snd_q;
	uint32_t	detached_snd_q_count;

	/* listener / passive-open bookkeeping */
	bool	closing;
	LIST_HEAD(, tcp) conninds;
	uint32_t next_connind_seq;

	LIST_ENTRY(tcp) conninds_entry;	/* valid for passive child */
	struct tcp *passive_listener;	/* retained while queued */
	uint32_t connind_seq;		/* T_CONN_IND / T_DISCON_IND seqno */
	bool	connind_sent;		/* T_CONN_IND already sent upward */

	enum tcp_state state;	/* state of TCB */
	int conn_id;		/* ID in the connections/binds table */

	struct sockaddr_in laddr;	/* local address */
	struct sockaddr_in faddr;	/* foreign address */

	struct tcp_timer {
		kcallout_t	callout;
		kdpc_t		dpc;
		kabstime_t	deadline;
	} timers[TCP_TIMER_MAX];
	atomic_uint pending_timers;

	bool	shutdown_rd:	1, /* can't receive any more. */
		shutdown_wr:	1, /* can't send any more */
		emit_ack:	1, /* send ACK next tcp_output() */
		ordrel_needed:	1, /* need to put up T_ORDREL_IND after data */

		/* upstream deliveries deferred to tcp_rsrv*/
		pending_conn_con:	1,
		pending_ordrel:		1,
		pending_discon:		1;
	int pending_discon_reason;

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
	tcp_seq_t	rcv_up;		/* receive urgent pointer */
	uint32_t	rcv_wnd_max;	/* maximum receive window */

	bool	timing_rtt;	/* is RTT being timed for a transmitted seg? */
	tcp_seq_t	rtseq;		/* seq of segment being timed for RTT */
	kabstime_t	rtstart;	/* start time of RTT measurement */

	uint32_t	srtt;		/* smoothed RTT (ms) */
	uint32_t	rttvar;		/* RTT variance (ms) */
	uint32_t	rto;		/* retransmission timeout (ms) */
	uint8_t		n_rexmits;	/* num of rexmits of current segment */

	mblk_q_t	reass_queue;	/* sergment reassembly queue */

	uint32_t	snd_cwnd;	/* congestion window */
	uint32_t	snd_ssthresh;	/* slow start threshold */
	uint32_t	bytes_acked;	/* bytes acknowledged */

	uint8_t		dupacks;	/* duplicate acknowledgments */
	tcp_seq_t	snd_recover;	/* sequence number to recover from */
	bool		fast_recovery;	/* in fast recovery? */

	mblk_t	*conn_con_m;	/* pre-allocated T_CONN_CON */
	mblk_t	*ordrel_ind_m;	/* pre-allocated T_ORDREL_IND */
	mblk_t	*discon_ind_m;	/* pre-allocated T_DISCON_IND */
	mblk_t	*conn_ind_m;	/* pre-allocated T_CONN_IND (passive opener) */

	krcu_entry_t rcu;
} tcp_t;

static int tcp_open(queue_t *, void *dev);
static void tcp_close(queue_t *);
static void tcp_wput(queue_t *, mblk_t *);
static void tcp_wsrv(queue_t *);
static void tcp_rsrv(queue_t *);

static int tcp_output(tcp_t *);

static void tcp_ordrel_ind(tcp_t *);
void tcp_conn_input(tcp_t **tpp, mblk_t *, const ip_rxattr_t *attr);

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
static kspinlock_t tcp_conntab_lock;

#define TCP_EPHEMERAL_LOW  49152
#define TCP_EPHEMERAL_HIGH 65535
static uint16_t tcp_next_ephemeral = TCP_EPHEMERAL_LOW;

#define TCP_TRACE(...) kdprintf("TCP: " __VA_ARGS__)

/*
 * TCB lifecycle
 */

static tcp_t *
tcp_new(queue_t *rq)
{
	tcp_t *tp = kmem_alloc(sizeof(*tp));
	if (tp == NULL)
		return NULL;

	ke_spinlock_init(&tp->lock);
	tp->refcnt = 1;

	tp->rq = rq;

	tp->pending_conn_con = false;
	tp->pending_ordrel = false;
	tp->pending_discon = false;
	tp->pending_discon_reason = 0;

	tp->state = TCPS_CLOSED;
	tp->conn_id = -1;

	tp->closing = false;
	LIST_INIT(&tp->conninds);
	tp->next_connind_seq = 1;
	tp->passive_listener = NULL;
	tp->connind_seq = 0;
	tp->connind_sent = false;

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
	tp->shutdown_wr = 0;
	tp->emit_ack = 0;
	tp->ordrel_needed = 0;

	TAILQ_INIT(&tp->rcv_q);
	tp->rcv_q_count = 0;

	TAILQ_INIT(&tp->detached_snd_q);
	tp->detached_snd_q_count = 0;

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
	kassert(atomic_fetch_add_explicit(&tp->refcnt, 1,
	    memory_order_relaxed) > 0);
}

static bool
tcp_tryretain(tcp_t *tp)
{
	unsigned int current = atomic_load_explicit(&tp->refcnt,
	    memory_order_acquire);
	for (;;) {
		if (current == 0)
			return false;
		if (atomic_compare_exchange_weak_explicit(&tp->refcnt, &current,
			current + 1, memory_order_acq_rel,
			memory_order_acquire))
			return true;
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
		ke_rcu_call(&tp->rcu, tcp_free_rcu, tp);
	}
}

/*
 * locking helpers
 */

#if 0 /* currently unused */
static ipl_t
tcp_enter_two(tcp_t *a, tcp_t *b)
{
	ipl_t ipl;
	kassert(a != b);
	if (a < b) {
		ipl = ke_spinlock_enter(&a->lock);
		ke_spinlock_enter_nospl(&b->lock);
	} else {
		ipl = ke_spinlock_enter(&b->lock);
		ke_spinlock_enter_nospl(&a->lock);
	}
	return ipl;
}
#endif

static void __attribute__((unused))
tcp_exit_two(tcp_t *a, tcp_t *b, ipl_t ipl)
{
	ke_spinlock_exit_nospl(&a->lock);
	ke_spinlock_exit_nospl(&b->lock);
	splx(ipl);
}

static ipl_t
tcp_enter_three(tcp_t *a, tcp_t *b, tcp_t *c)
{
	tcp_t *v[3] = { a, b, c };
	tcp_t *t;

	kassert(a != b && b != c && a != c);

	if (v[0] > v[1]) { t = v[0]; v[0] = v[1]; v[1] = t; }
	if (v[1] > v[2]) { t = v[1]; v[1] = v[2]; v[2] = t; }
	if (v[0] > v[1]) { t = v[0]; v[0] = v[1]; v[1] = t; }

	ipl_t ipl = ke_spinlock_enter(&v[0]->lock);
	ke_spinlock_enter_nospl(&v[1]->lock);
	ke_spinlock_enter_nospl(&v[2]->lock);
	return ipl;
}

static void
tcp_exit_three(tcp_t *a, tcp_t *b, tcp_t *c, ipl_t ipl)
{
	ke_spinlock_exit_nospl(&a->lock);
	ke_spinlock_exit_nospl(&b->lock);
	ke_spinlock_exit_nospl(&c->lock);
	splx(ipl);
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
		ipl_t ipl = ke_spinlock_enter(&tcp_conntab_lock);
		kassert(tcp_conn_table[tp->conn_id] == tp);
		tcp_conn_table[tp->conn_id] = NULL;
		ke_spinlock_exit(&tcp_conntab_lock, ipl);
		tp->conn_id = -1;
	}

	str_mblk_q_free(&tp->reass_queue);

	str_mblk_q_free(&tp->detached_snd_q);
	tp->detached_snd_q_count = 0;

	str_mblk_q_free(&tp->rcv_q);
	tp->rcv_q_count = 0;

	str_freeb(tp->conn_con_m);
	tp->conn_con_m = NULL;
	str_freeb(tp->ordrel_ind_m);
	tp->ordrel_ind_m = NULL;
	if (!tp->pending_discon) {
		str_freeb(tp->discon_ind_m);
		tp->discon_ind_m = NULL;
	}
	str_freeb(tp->conn_ind_m);
	tp->conn_ind_m = NULL;

	tp->connind_sent = false;
	tp->connind_seq = 0;
}

static queue_t *
tcp_wq(tcp_t *tp)
{
	return tp->rq != NULL ? tp->rq->other : NULL;
}

/*
 * detach the TCB from its stream. steals the write queue into the
 * detached send queue so tcp_output can still retransmit.
 */
static void
tcp_detach(tcp_t *tp)
{
	kassert(TAILQ_EMPTY(&tp->detached_snd_q));
	kassert(tp->detached_snd_q_count == 0);

	TAILQ_CONCAT(&tp->detached_snd_q, &tcp_wq(tp)->msgq, link);
	tp->detached_snd_q_count = tcp_wq(tp)->count;
	tcp_wq(tp)->count = 0;
	TAILQ_INIT(&tcp_wq(tp)->msgq);

	tp->rq = NULL;
}

/*
 * attach a passive child to an acceptor's stream.
 * all three TCBs' locks held.
 */
static void
tcp_attach(tcp_t *child, tcp_t *acceptor)
{
	queue_t *rq;

	kassert(child->rq == NULL);
	kassert(acceptor->rq != NULL);

	rq = acceptor->rq;
	kassert(rq->ptr == acceptor);
	kassert(rq->other->ptr == acceptor);
	kassert(TAILQ_EMPTY(&rq->msgq));
	kassert(TAILQ_EMPTY(&child->detached_snd_q));

	acceptor->rq = NULL;

	rq->ptr = rq->other->ptr = child;
	child->rq = rq;

	if (!TAILQ_EMPTY(&child->rcv_q) || child->pending_ordrel ||
	    child->pending_conn_con || child->pending_discon)
		str_qenable(rq);

	if (atomic_load(&child->pending_timers) != 0)
		str_qenable(rq->other);

	str_kick(rq->stdata);
}

/*
 * output processing
 */

static int
tcp_open(queue_t *rq, void *devp)
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
	kassert(ke_spinlock_held(&listener->lock));
	kassert(ke_spinlock_held(&child->lock));
	kassert(child->passive_listener == listener);

	LIST_REMOVE(child, conninds_entry);
	child->passive_listener = NULL;

	tcp_release(child);	 /* listener list ref on child */
	tcp_release(listener); /* child ref on listener */
}

static void
tcp_close(queue_t *rq)
{
	tcp_t *tp = rq->ptr;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&tp->lock);

	TCP_TRACE("closing connection TCB %p state %s\n", tp,
	    tcp_state_names[tp->state]);

	switch (tp->state) {
	case TCPS_LISTEN: {
		for (;;) {
			tcp_t *child;

			if (tp->conn_id != -1) {
				ke_spinlock_enter_nospl(&tcp_conntab_lock);
				if (tcp_conn_table[tp->conn_id] == tp)
					tcp_conn_table[tp->conn_id] = NULL;
				ke_spinlock_exit_nospl(&tcp_conntab_lock);
				tp->conn_id = -1;
			}

			tp->closing = true;

			child = LIST_FIRST(&tp->conninds);
			if (child == NULL)
				break;

			tcp_retain(child);

			if (tp > child) {
				ke_spinlock_exit_nospl(&tp->lock);
				ke_spinlock_enter_nospl(&child->lock);
				ke_spinlock_enter_nospl(&tp->lock);
			} else {
				ke_spinlock_enter_nospl(&child->lock);
			}

			if (child->passive_listener == tp) {
				tcp_passive_unlink(tp, child);
				tcb_free_connstate(child);
				ke_spinlock_exit_nospl(&child->lock);
				tcp_release(child); /* child existence ref */
			} else {
				ke_spinlock_exit_nospl(&child->lock);
			}

			tcp_release(child);
		}

		tcp_detach(tp);
		tcb_free_connstate(tp);
		ke_spinlock_exit(&tp->lock, ipl);
		tcp_release(tp);
		break;
	}

	case TCPS_CLOSED:
	case TCPS_BOUND:
	case TCPS_SYN_SENT:
		tcp_detach(tp);
		tcb_free_connstate(tp);
		ke_spinlock_exit(&tp->lock, ipl);
		tcp_release(tp);
		break;

	case TCPS_SYN_RECEIVED:
	case TCPS_ESTABLISHED:
		if (!TAILQ_EMPTY(&tp->reass_queue) || !TAILQ_EMPTY(&tp->rcv_q))
			kfatal("TODO: tcp_close: unreceived data on close\n");

		tcp_detach(tp);
		tcp_change_state(tp, TCPS_FIN_WAIT_1);
		tcp_output(tp);
		ke_spinlock_exit(&tp->lock, ipl);
		break;

	case TCPS_CLOSE_WAIT:
		if (!TAILQ_EMPTY(&tp->reass_queue) || !TAILQ_EMPTY(&tp->rcv_q))
			kfatal("TODO: tcp_close: unreceived data on close\n");

		tcp_detach(tp);
		tcp_change_state(tp, TCPS_LAST_ACK);
		tcp_output(tp);
		ke_spinlock_exit(&tp->lock, ipl);
		break;

	case TCPS_FIN_WAIT_1:
	case TCPS_CLOSING:
	case TCPS_LAST_ACK:
	case TCPS_FIN_WAIT_2:
	case TCPS_TIME_WAIT:
		kassert(TAILQ_EMPTY(&tp->reass_queue) && TAILQ_EMPTY(&tp->rcv_q));
		tcp_detach(tp);
		ke_spinlock_exit(&tp->lock, ipl);
		break;
	}
}

static size_t
tcp_snd_q_count(tcp_t *tp)
{
	if (tp->rq == NULL)
		return tp->detached_snd_q_count;
	else
		return tcp_wq(tp)->count;
}

/*
 * connection/binding management
 */

static bool
tcp_port_in_use(uint16_t port, in_addr_t addr)
{
	kassert(ke_spinlock_held(&tcp_conntab_lock));
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL && t->laddr.sin_port == port &&
		    (t->laddr.sin_addr.s_addr == INADDR_ANY ||
			addr == INADDR_ANY ||
			t->laddr.sin_addr.s_addr == addr))
			return true;
	}
	return false;
}

static uint16_t
tcp_alloc_ephemeral(in_addr_t addr)
{
	uint16_t start = tcp_next_ephemeral;

	kassert(ke_spinlock_held(&tcp_conntab_lock));

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
tcp_conn_table_insert_locked(tcp_t *tp)
{
	kassert(ke_spinlock_held(&tcp_conntab_lock));

	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		if (tcp_conn_table[i] == NULL) {
			tcp_conn_table[i] = tp;
			tp->conn_id = i;
			return 0;
		}
	}
	return -ENOSPC;
}

static tcp_t *
tcp_conn_lookup_locked(struct in_addr src, uint16_t sport,
    struct in_addr dst, uint16_t dport)
{
	kassert(ke_spinlock_held(&tcp_conntab_lock));

	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL &&
		    t->laddr.sin_addr.s_addr == dst.s_addr &&
		    t->laddr.sin_port == dport &&
		    t->faddr.sin_addr.s_addr == src.s_addr &&
		    t->faddr.sin_port == sport)
			return t;
	}

	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_t *t = tcp_conn_table[i];
		if (t != NULL && t->state == TCPS_LISTEN &&
		    t->laddr.sin_port == dport &&
		    (t->laddr.sin_addr.s_addr == INADDR_ANY ||
			t->laddr.sin_addr.s_addr == dst.s_addr))
			return t;
	}

	return NULL;
}

static int
tcp_do_bind(tcp_t *tp, struct sockaddr_in *laddr)
{
	uint16_t port;
	int r;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&tcp_conntab_lock);

	if (laddr->sin_port == 0) {
		port = tcp_alloc_ephemeral(laddr->sin_addr.s_addr);
		if (port == 0) {
			ke_spinlock_exit(&tcp_conntab_lock, ipl);
			return -EADDRINUSE;
		}
	} else {
		if (tcp_port_in_use(laddr->sin_port, laddr->sin_addr.s_addr)) {
			ke_spinlock_exit(&tcp_conntab_lock, ipl);
			return -EADDRINUSE;
		}
		port = laddr->sin_port;
	}

	tp->laddr.sin_family = AF_INET;
	tp->laddr.sin_port = port;
	tp->laddr.sin_addr = laddr->sin_addr;

	r = tcp_conn_table_insert_locked(tp);
	if (r != 0) {
		ke_spinlock_exit(&tcp_conntab_lock, ipl);
		return r;
	}

	tcp_change_state(tp, TCPS_BOUND);

	ke_spinlock_exit(&tcp_conntab_lock, ipl);
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

	if (tp->mss > 2190)
		tp->snd_cwnd = 2 * tp->mss;
	else if (tp->mss > 1095)
		tp->snd_cwnd = 3 * tp->mss;
	else
		tp->snd_cwnd = 4 * tp->mss;
	tp->snd_ssthresh = INT32_MAX;
	tp->snd_recover = tp->iss;

	tp->bytes_acked = 0;
	tp->dupacks = 0;
	tp->fast_recovery = false;

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

/*
 * STREAMS output processing
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

static void
tcp_wput_bind_req(queue_t *wq, mblk_t *mp)
{
	tcp_t *tp = wq->ptr;
	struct T_bind_req *br = (struct T_bind_req *)mp->rptr;
	struct sockaddr_in sin;
	ipl_t ipl;
	int r;

	ipl = ke_spinlock_enter(&tp->lock);

	if (!(tp->state == TCPS_CLOSED) &&
	    !(tp->state == TCPS_BOUND && br->CONIND_number > 0)) {
		ke_spinlock_exit(&tp->lock, ipl);
		reply_error_ack(wq, mp, T_BIND_REQ, EINVAL);
		return;
	}

	if (br->ADDR_length >= (int)sizeof(struct sockaddr_in)) {
		sin = *(struct sockaddr_in *)&br->ADDR;
	} else {
		sin.sin_family = AF_INET;
		sin.sin_port = 0;
		sin.sin_addr.s_addr = INADDR_ANY;
	}

	if (tp->state == TCPS_CLOSED) {
		r = tcp_do_bind(tp, &sin);
		if (r != 0) {
			ke_spinlock_exit(&tp->lock, ipl);
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

	ke_spinlock_exit(&tp->lock, ipl);

	br->PRIM_type = T_BIND_ACK;
	str_qreply(wq, mp);
}

static void
tcp_wput_conn_req(queue_t *wq, mblk_t *mp)
{
	struct T_conn_req *cr = (struct T_conn_req *)mp->rptr;
	struct sockaddr_in *dest = (struct sockaddr_in *)&cr->DEST;
	tcp_t *tp = wq->ptr;
	ipl_t ipl;
	int r;

	ipl = ke_spinlock_enter(&tp->lock);

	switch (tp->state) {
	case TCPS_CLOSED: {
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_port = 0,
			.sin_addr.s_addr = INADDR_ANY,
		};

		r = tcp_do_bind(tp, &sin);
		if (r != 0) {
			ke_spinlock_exit(&tp->lock, ipl);
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}
		/* fall through */
	}

	case TCPS_BOUND:
		if (tp->laddr.sin_addr.s_addr == INADDR_ANY) {
			union sockaddr_union rt_dst = {
				.in = { .sin_family = AF_INET,
					.sin_addr = dest->sin_addr }
			};
			route_result_t rt;
			ip_ifaddr_t *ifa;
			uint32_t mtu;

			r = route_lookup(&rt_dst, &rt, true);
			if (r != 0) {
				ke_spinlock_exit(&tp->lock, ipl);
				reply_error_ack(wq, mp, T_CONN_REQ, EHOSTUNREACH);
				return;
			}

			/* spinlock is held, so in an RCU grace period */
			RCULIST_FOREACH(ifa, &rt.ifp->addrs, rlentry) {
				if (ifa->addr.sa.sa_family == AF_INET) {
					tp->laddr.sin_addr =
					    ifa->addr.in.sin_addr;
					break;
				}
			}

#if 0 /* when we have PMTUD */
			mtu = rt.mtu != 0 ? rt.mtu : TCP_DEFAULT_MTU;
			tp->mss = mtu - sizeof(struct ether_header) -
			    sizeof(struct ip) - sizeof(struct tcphdr);
#endif
			tp->mss = TCP_MSS;
			ip_if_release(rt.ifp);
		}

		r = tcp_setup_connection(tp, dest);
		if (r != 0) {
			ke_spinlock_exit(&tp->lock, ipl);
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}

		tcp_change_state(tp, TCPS_SYN_SENT);
		tcp_output(tp);

		ke_spinlock_exit(&tp->lock, ipl);

		reply_ok_ack(wq, mp, T_CONN_REQ);
		break;

	default:
		TCP_TRACE("tcp_connect: invalid state %d\n", tp->state);
		ke_spinlock_exit(&tp->lock, ipl);
		reply_error_ack(wq, mp, T_CONN_REQ, EISCONN);
		return;
	}
}

/* FIXME: failure handling - get rid of the eager and the connind! */
static void
tcp_wput_conn_res(queue_t *wq, mblk_t *mp)
{
	tcp_t *listener = wq->ptr;
	struct T_conn_res *cres = (struct T_conn_res *)mp->rptr;
	queue_t *acceptorrq;
	tcp_t *child = NULL, *acceptor;
	ipl_t ipl;
	bool ok = false;

	/* sockfs guarantees continued existence of the acceptor stream */
	acceptorrq = (queue_t *)cres->ACCEPTOR_id;
	kassert(acceptorrq->qinfo == &tcp_rinit);

	ipl = ke_spinlock_enter(&listener->lock);
	if (listener->state != TCPS_LISTEN || listener->closing) {
		ke_spinlock_exit(&listener->lock, ipl);
		reply_error_ack(wq, mp, T_CONN_RES, EINVAL);
		return;
	}

	LIST_FOREACH(child, &listener->conninds, conninds_entry)
		if (child->connind_seq == cres->SEQ_number)
			break;
	if (child != NULL)
		tcp_retain(child);

	ke_spinlock_exit(&listener->lock, ipl);

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

	ipl = tcp_enter_three(listener, child, acceptor);

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
	kassert(TAILQ_EMPTY(&acceptor->rcv_q));

	tcp_passive_unlink(listener, child);

	tcb_free_connstate(acceptor);
	tcp_attach(child, acceptor);

	ok = true;

out:
	tcp_exit_three(listener, child, acceptor, ipl);

	if (ok) {
		tcp_release(acceptor);	/* drop old acceptor ref from q->ptr */
		tcp_release(child);	/* drop local ref from search */
		reply_ok_ack(wq, mp, T_CONN_RES);
	} else {
		tcp_release(child);
		reply_error_ack(wq, mp, T_CONN_RES, EINVAL);
	}
}

static void
tcp_wput_addr_req(queue_t *wq, mblk_t *mp)
{
	tcp_t *tp = wq->ptr;
	struct T_addr_ack *aa;
	struct sockaddr_in *sin;
	mblk_t *ackmp;
	ipl_t ipl;

	ackmp = str_allocb(sizeof(struct T_addr_ack));
	if (ackmp == NULL) {
		reply_error_ack(wq, mp, T_ADDR_REQ, ENOMEM);
		return;
	}

	str_freemsg(mp);

	ackmp->db->type = M_PCPROTO;
	aa = (struct T_addr_ack *)ackmp->rptr;
	aa->PRIM_type = T_ADDR_ACK;

	ipl = ke_spinlock_enter(&tp->lock);

	if (tp->state >= TCPS_BOUND) {
		aa->LOCADDR_length = sizeof(struct sockaddr_in);
		sin = (struct sockaddr_in *)&aa->LOCADDR;
		*sin = tp->laddr;
	} else {
		aa->LOCADDR_length = 0;
	}

	if (tp->state >= TCPS_SYN_SENT) {
		aa->REMADDR_length = sizeof(struct sockaddr_in);
		sin = (struct sockaddr_in *)&aa->REMADDR;
		*sin = tp->faddr;
	} else {
		aa->REMADDR_length = 0;
	}

	ke_spinlock_exit(&tp->lock, ipl);

	ackmp->wptr = ackmp->rptr + sizeof(struct T_addr_ack);
	str_qreply(wq, ackmp);
}

static void
tcp_wput_ordrel_req(queue_t *wq, mblk_t *mp)
{
	tcp_t *tp = wq->ptr;
	ipl_t ipl;
	int err = 0;

	ipl = ke_spinlock_enter(&tp->lock);

	switch (tp->state) {
	case TCPS_ESTABLISHED:
		if (tp->shutdown_wr) {
			err = EINVAL;
			break;
		}
		tp->shutdown_wr = true;
		tcp_change_state(tp, TCPS_FIN_WAIT_1);
		tcp_output(tp);
		break;

	case TCPS_CLOSE_WAIT:
		if (tp->shutdown_wr) {
			err = EINVAL;
			break;
		}
		tp->shutdown_wr = true;
		tcp_change_state(tp, TCPS_LAST_ACK);
		tcp_output(tp);
		break;

	default:
		err = EINVAL;
		break;
	}

	ke_spinlock_exit(&tp->lock, ipl);

	if (err != 0)
		reply_error_ack(wq, mp, T_ORDREL_REQ, err);
	else
		reply_ok_ack(wq, mp, T_ORDREL_REQ);
}

static void
tcp_wput_data(queue_t *wq, mblk_t *mp)
{
	tcp_t *tp = wq->ptr;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&tp->lock);

	if (tp->shutdown_wr) {
		ke_spinlock_exit(&tp->lock, ipl);
		str_freemsg(mp);
		return;
	}

	str_putq(wq, mp);
	tcp_output(tp);

	ke_spinlock_exit(&tp->lock, ipl);
}

static void
tcp_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_DATA:
		tcp_wput_data(wq, mp);
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

		case T_ADDR_REQ:
			tcp_wput_addr_req(wq, mp);
			break;

		case T_ORDREL_REQ:
			tcp_wput_ordrel_req(wq, mp);
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
tcp_dispatch_pending_timers(tcp_t *tp, uint32_t pending)
{
	if (pending & (1 << TCP_TIMER_REXMT))
		tcp_rexmt_timer(tp);
	if (pending & (1 << TCP_TIMER_2MSL))
		tcp_2msl_timer(tp);
	if (pending & (1 << TCP_TIMER_KEEPALIVE))
		tcp_keepalive_timer(tp);
	if (pending & (1 << TCP_TIMER_PERSIST))
		tcp_persist_timer(tp);
}

static void
tcp_wsrv(queue_t *wq)
{
	tcp_t *tp = wq->ptr;
	ipl_t ipl;
	uint32_t pending;

	ipl = ke_spinlock_enter(&tp->lock);
	pending = atomic_exchange(&tp->pending_timers, 0);
	if (pending != 0)
		tcp_dispatch_pending_timers(tp, pending);
	ke_spinlock_exit(&tp->lock, ipl);
}

/*
 * tcp_rsrv: drain rcv_q and deliver pending upstream indications.
 * Runs at thread level (stream service context), safe to call str_putnext.
 */
static void
tcp_rsrv(queue_t *rq)
{
	tcp_t *tp = rq->ptr;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&tp->lock);
	if (tp->pending_discon) {
		mblk_t *mp;

		tp->pending_discon = false;
		mp = tp->discon_ind_m;
		tp->discon_ind_m = NULL;
		ke_spinlock_exit(&tp->lock, ipl);

		str_putnext(rq, mp);
		return;
	}

	/* active open completed */
	if (tp->pending_conn_con) {
		mblk_t *mp;

		tp->pending_conn_con = false;
		mp = tp->conn_con_m;
		tp->conn_con_m = NULL;
		ke_spinlock_exit(&tp->lock, ipl);

		str_putnext(rq, mp);

		return;
	}
	ke_spinlock_exit(&tp->lock, ipl);

	for (;;) {
		mblk_t *mp;

		if (!str_canputnext(rq))
			return;

		ipl = ke_spinlock_enter(&tp->lock);
		mp = TAILQ_FIRST(&tp->rcv_q);
		if (mp != NULL) {
			TAILQ_REMOVE(&tp->rcv_q, mp, link);
			tp->rcv_q_count -= str_msgsize(mp);
		}
		ke_spinlock_exit(&tp->lock, ipl);

		if (mp == NULL)
			break;

		str_putnext(rq, mp);
	}

	if (str_canputnext(rq)) {
		size_t wnd_opening;

		ipl = ke_spinlock_enter(&tp->lock);

		/* ordrel can go up if no data left in queue */
		if (tp->pending_ordrel) {
			mblk_t *mp;

			tp->pending_ordrel = false;
			mp = tp->ordrel_ind_m;
			tp->ordrel_ind_m = NULL;
			ke_spinlock_exit(&tp->lock, ipl);

			str_putnext(rq, mp);
			return;
		}

		wnd_opening = tp->rcv_wnd_max - tp->rcv_wnd;
		tp->rcv_wnd = tp->rcv_wnd_max;

		if (wnd_opening >= (size_t)(tp->mss * 2) ||
		    wnd_opening >= tp->rcv_wnd_max / 2) {
			tp->emit_ack = true;
			tcp_output(tp);
		}

		ke_spinlock_exit(&tp->lock, ipl);
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

static uint16_t
tcp_checksum(const struct ip *ip, const struct tcphdr *th, size_t tcp_len)
{
	uint8_t pseudo[12];
	uint32_t sum = 0;

	memcpy(&pseudo[0], &ip->ip_src.s_addr, 4);
	memcpy(&pseudo[4], &ip->ip_dst.s_addr, 4);
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_TCP;
	pseudo[10] = (tcp_len >> 8) & 0xff;
	pseudo[11] = tcp_len & 0xff;

	sum = csum_add(sum, pseudo, sizeof(pseudo));
	sum = csum_add(sum, (const uint8_t *)th, tcp_len);

	return csum_finish(sum);
}

static void
tcp_reply(const struct ip *orig_ip, const struct tcphdr *orig_th,
    tcp_seq_t seq, tcp_seq_t ack, uint8_t flags)
{
	mblk_t *mp;
	struct ip *ip;
	struct tcphdr *th;

	mp = str_allocb(sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr));
	if (mp == NULL)
		return;

	mp->rptr += sizeof(struct ether_header);
	mp->wptr += sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr);
	ip = (struct ip *)mp->rptr;
	th = (struct tcphdr *)(ip + 1);

	ip->ip_v = IPVERSION;
	ip->ip_hl = sizeof(struct ip) >> 2;
	ip->ip_tos = 0;
	ip->ip_len = htons(sizeof(struct ip) + sizeof(struct tcphdr));
	ip->ip_id = 0;
	ip->ip_off = 0;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_TCP;
	ip->ip_sum = 0;
	ip->ip_src = orig_ip->ip_dst;
	ip->ip_dst = orig_ip->ip_src;

	th->th_sport = orig_th->th_dport;
	th->th_dport = orig_th->th_sport;
	th->th_seq = htonl(seq);
	th->th_ack = htonl(ack);
	th->th_off = sizeof(struct tcphdr) >> 2;
	th->th_x2 = 0;
	th->th_flags = flags;
	th->th_win = 0;
	th->th_urp = 0;
	th->th_sum = 0;
	th->th_sum = htons(tcp_checksum(ip, th, sizeof(struct tcphdr)));

	ipv4_output(mp);
}

static void
tcp_reply_reset(const struct ip *ip, const struct tcphdr *th, uint16_t tcp_len)
{
	if (th->th_flags & TH_ACK) {
		tcp_reply(ip, th, ntohl(th->th_ack), 0, TH_RST);
	} else {
		size_t data_len = tcp_len - (th->th_off << 2);
		if (th->th_flags & TH_SYN)
			data_len++;
		if (th->th_flags & TH_FIN)
			data_len++;
		tcp_reply(ip, th, 0, ntohl(th->th_seq) + data_len,
		    TH_RST | TH_ACK);
	}
}

/*
 * Output processing
 */

static size_t
copy_from_mpchain(mblk_t *m, size_t off, size_t len, uint8_t *dst)
{
	size_t copied = 0;

	for (; m != NULL && len != 0; m = m->cont) {
		size_t blen = (size_t)(m->wptr - m->rptr);

		if (off >= blen) {
			off -= blen;
			continue;
		}

		size_t avail = blen - off;
		size_t take = MIN2(avail, len);

		memcpy(dst + copied, m->rptr + off, take);
		copied += take;
		len -= take;
		off = 0;
	}

	return copied;
}

static size_t
copy_data(tcp_t *tp, size_t off, size_t len, uint8_t *dst)
{
	mblk_q_t *q;
	mblk_t *m;
	size_t copied = 0;

	if (len == 0)
		return 0;

	if (tcp_wq(tp) != NULL)
		q = &tcp_wq(tp)->msgq;
	else
		q = &tp->detached_snd_q;

	TAILQ_FOREACH(m, q, link) {
		size_t msglen = str_msgsize(m);
		size_t take;
		size_t n;

		if (off >= msglen) {
			off -= msglen;
			continue;
		}

		take = MIN2(msglen - off, len);
		n = copy_from_mpchain(m, off, take, dst + copied);

		kassert(n == take);

		copied += n;
		len -= n;
		off = 0;

		if (len == 0)
			break;
	}

	return copied;
}

static int
do_send(tcp_t *tp, tcp_seq_t seq, uint8_t flags, size_t data_len,
    size_t data_off)
{
	mblk_t *mp;
	struct ip *ip;
	struct tcphdr *th;

	mp = str_allocb(sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr) + data_len);
	if (mp == NULL)
		return -ENOMEM;

	mp->rptr += sizeof(struct ether_header);
	mp->wptr += sizeof(struct ether_header) + sizeof(struct ip) +
	    sizeof(struct tcphdr) + data_len;

	ip = (struct ip *)mp->rptr;
	th = (struct tcphdr *)(ip + 1);

	ip->ip_v = IPVERSION;
	ip->ip_hl = sizeof(struct ip) >> 2;
	ip->ip_tos = 0;
	ip->ip_len = htons(sizeof(struct ip) + sizeof(struct tcphdr) + data_len);
	ip->ip_id = htons(0);
	ip->ip_off = 0;
	ip->ip_ttl = 64;
	ip->ip_p = IPPROTO_TCP;
	ip->ip_sum = 0;
	ip->ip_src.s_addr = tp->laddr.sin_addr.s_addr;
	ip->ip_dst.s_addr = tp->faddr.sin_addr.s_addr;

	th->th_sport = tp->laddr.sin_port;
	th->th_dport = tp->faddr.sin_port;
	th->th_seq = htonl(seq);
	th->th_ack = htonl(tp->rcv_nxt);
	th->th_off = sizeof(struct tcphdr) >> 2;
	th->th_flags = flags;
	th->th_win = htons(tp->rcv_wnd);
	th->th_x2 = 0;
	th->th_urp = 0;

	kassert(copy_data(tp, data_off, data_len, (uint8_t *)(th + 1)) ==
	    data_len);

	th->th_sum = 0;
	th->th_sum = htons(tcp_checksum(ip, th, sizeof(struct tcphdr) +
	    data_len));

	ipv4_output(mp);

	return 0;
}

static uint8_t tcp_out_flags[] = {
	[TCPS_CLOSED]		= TH_RST | TH_ACK,
	[TCPS_BOUND] 		= TH_RST | TH_ACK,
	[TCPS_LISTEN]		= 0,
	[TCPS_SYN_SENT]		= TH_SYN,
	[TCPS_SYN_RECEIVED]	= TH_SYN | TH_ACK,
	[TCPS_ESTABLISHED]	= TH_ACK,
	[TCPS_CLOSE_WAIT]	= TH_ACK,
	[TCPS_FIN_WAIT_1]	= TH_FIN | TH_ACK,
	[TCPS_CLOSING]		= TH_FIN | TH_ACK,
	[TCPS_LAST_ACK]		= TH_FIN | TH_ACK,
	[TCPS_FIN_WAIT_2]	= TH_ACK,
	[TCPS_TIME_WAIT]	= TH_ACK,
};

int
tcp_output(tcp_t *tp)
{
	uint8_t flags;
	int data_len, data_off;
	int swnd;
	uint32_t flight;
	bool can_send_more;
	tcp_seq_t old_nxt;
	int r;

	kassert(ke_spinlock_held(&tp->lock));

	/*
	 * These are the reasons to send a packet:
	 * 1. There are SYN, RST, or FIN flags to be sent.
	 * 2. We should ACK.
	 * 3. Data is available and Nagle's algorithm allows us to send it.
	 */

send_more:
	flags = tcp_out_flags[tp->state];
	can_send_more = false;
	old_nxt = tp->snd_nxt;

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

	flight = tp->snd_max - tp->snd_una;
	data_len = tcp_snd_q_count(tp);
	data_off = tp->snd_nxt - tp->snd_una;
	data_len -= data_off;
	swnd = MIN2(tp->snd_wnd, tp->snd_cwnd) - flight;

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
	 * Also sending FIN.
	 */
	if (data_len != 0 && (flags & TH_FIN) == 0) {
		if (tp->snd_wnd < tp->mss || data_len < tp->mss) {
			if (tp->snd_una != tp->snd_max) {
				TCP_TRACE("nagle delaying send\n");
				data_len = 0;
			}
		}
	}

	/* If we're not sending everything in the queue, clear FIN. */
	if (data_off + data_len < tcp_snd_q_count(tp))
		flags &= ~TH_FIN;
	/* and if we already sent FIN, don't resend it */
	else if (SEQ_GT(tp->snd_nxt, tp->snd_una + tcp_snd_q_count(tp)))
		flags &= ~TH_FIN;
	else if (data_len != 0)
		flags |= TH_PUSH;

	if (tp->emit_ack) {
		tp->emit_ack = false;
		r = do_send(tp, tp->snd_nxt, flags, data_len, data_off);
	} else if ((flags & (TH_SYN | TH_RST | TH_FIN)) || data_len != 0) {
		r = do_send(tp, tp->snd_nxt, flags, data_len, data_off);
	} else {
		return 0;
	}

	tp->snd_nxt += data_len;
	if (flags & TH_SYN)
		tp->snd_nxt++;
	if (flags & TH_FIN)
		tp->snd_nxt++;

	if (SEQ_GT(tp->snd_nxt, tp->snd_max)) {
		/* newly-sent data/control */
		tp->snd_max = tp->snd_nxt;

		if (!tp->timing_rtt) {
			tp->timing_rtt = true;
			tp->rtstart = ke_time();
			tp->rtseq = old_nxt;
		}
	}

	/* if sending other than a pure ACK, ensure rexmt timer is on */
	if ((data_len != 0 || (flags & (TH_SYN | TH_FIN))) &&
	    (tp->timers[TCP_TIMER_REXMT].deadline == ABSTIME_NEVER))
		tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);

	if (r == 0 && can_send_more)
		goto send_more;

	return r;
}

/*
 * TODO: what if the peer retracts window? this does not respect that case.
 * Need to respect effective window here (and congestion window too!)
 * If we get a zero-window then retransmits pending or not, switch to
 * persistence, and proceed to retransmission only after window reopens.
 *
 * (actually, RFC 9293 3.8.6 says: "The sender MAY also retransmit old data
 * beyond SND.UNA+SND.WND (MAY-7), but SHOULD NOT time out the connection if
 * data beyond the right window edge is not acknowledged (SHLD-17).
 * If the window shrinks to zero, the TCP implementation MUST probe it in the
 * standard way (described below) (MUST-35)")
 */
static int
tcp_rexmit(tcp_t *tp)
{
	uint8_t flags;
	uint32_t outstanding, data_out;
	size_t rexmit_len;

	kassert(ke_spinlock_held(&tp->lock));

	outstanding = tp->snd_max - tp->snd_una;
	if (outstanding == 0)
		return 0;

	flags = tcp_out_flags[tp->state];

	data_out = MIN2(outstanding, (uint32_t)tcp_snd_q_count(tp));
	rexmit_len = MIN2((size_t)data_out, (size_t)tp->mss);

	/*
	 * only retransmit fin if it is unacknowledged, AND this rexmit includes
	 * all queued data preceding it
	 */
	if (SEQ_LEQ(tp->snd_max, tp->snd_una + tcp_snd_q_count(tp)) ||
	    rexmit_len < data_out)
		flags &= ~TH_FIN;

	if ((flags & (TH_SYN | TH_FIN)) == 0 && rexmit_len == 0)
		return 0;

	return do_send(tp, tp->snd_una, flags, rexmit_len, 0);
}

/*
 * input processing
 */

static void
tcp_conn_con(tcp_t *tp, const struct ip *ip, const struct tcphdr *th,
    in_port_t sport)
{
	struct T_conn_con *con;
	struct sockaddr_in *sin;

	kassert(tp->rq != NULL);
	kassert(tp->conn_con_m != NULL);

	con = (typeof(con))tp->conn_con_m->rptr;
	con->PRIM_type = T_CONN_CON;
	con->RES_length = sizeof(struct sockaddr_in);
	sin = (typeof(sin))&con->RES;
	sin->sin_family = AF_INET;
	sin->sin_port = sport;
	sin->sin_addr = ip->ip_src;

	tp->pending_conn_con = true;
}

static void
tcp_conn_ind(tcp_t *listener, tcp_t *child)
{
	mblk_t *mp;
	struct T_conn_ind *ci;
	struct sockaddr_in *sin;

	kassert(ke_spinlock_held(&listener->lock));
	kassert(ke_spinlock_held(&child->lock));
	kassert(listener->rq != NULL);
	kassert(child->conn_ind_m != NULL);
	kassert(!child->connind_sent);

	mp = child->conn_ind_m;
	child->conn_ind_m = NULL;
	child->connind_sent = true;

	mp->db->type = M_PROTO;
	ci = (typeof(ci))mp->rptr;
	ci->PRIM_type = T_CONN_IND;
	ci->SEQ_number = child->connind_seq;
	ci->SRC_length = sizeof(struct sockaddr_in);

	sin = (typeof(sin))&ci->SRC;
	sin->sin_family = AF_INET;
	sin->sin_port = child->faddr.sin_port;
	sin->sin_addr = child->faddr.sin_addr;

	mp->wptr = mp->rptr + sizeof(*ci);

	str_ingress_putq(listener->rq->stdata, mp);
}

static void
tcp_discon_ind_listener(tcp_t *listener, tcp_t *child, int reason)
{
	mblk_t *mp;
	struct T_discon_ind *di;

	kassert(ke_spinlock_held(&listener->lock));
	kassert(ke_spinlock_held(&child->lock));
	kassert(listener->rq != NULL);
	kassert(child->discon_ind_m != NULL);

	mp = child->discon_ind_m;
	child->discon_ind_m = NULL;

	mp->db->type = M_PROTO;
	di = (typeof(di))mp->rptr;
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

	kassert(ke_spinlock_held(&child->lock));

	if (child->passive_listener == NULL || child->connind_sent ||
	    child->conn_ind_m == NULL || child->state < TCPS_ESTABLISHED)
		return;

	listener = child->passive_listener;
	tcp_retain(listener); /* local ref while we may relock */

	if (child > listener) {
		ke_spinlock_exit_nospl(&child->lock);
		ke_spinlock_enter_nospl(&listener->lock);
		ke_spinlock_enter_nospl(&child->lock);
	} else {
		ke_spinlock_enter_nospl(&listener->lock);
	}

	if (child->passive_listener == listener &&
	    child->state >= TCPS_ESTABLISHED &&
	    !child->connind_sent &&
	    !listener->closing &&
	    child->conn_ind_m != NULL)
		tcp_conn_ind(listener, child);

	ke_spinlock_exit_nospl(&listener->lock);
	tcp_release(listener);
}

/* sets ordrel pending; tcp_rsrv() will deliver it duly */
static void
tcp_ordrel_ind(tcp_t *tp)
{
	struct T_ordrel_ind *ord;

	kassert(tp->ordrel_ind_m != NULL);

	ord = (typeof(ord))tp->ordrel_ind_m->rptr;
	ord->PRIM_type = T_ORDREL_IND;

	tp->pending_ordrel = true;
}

static void
tcp_abort(tcp_t *tp, int err)
{
	kassert(ke_spinlock_held(&tp->lock));

	if (tp->passive_listener != NULL) {
		tcp_t *listener = tp->passive_listener;
		tcp_retain(listener);

		if (tp > listener) {
			ke_spinlock_exit_nospl(&tp->lock);
			ke_spinlock_enter_nospl(&listener->lock);
			ke_spinlock_enter_nospl(&tp->lock);
		} else {
			ke_spinlock_enter_nospl(&listener->lock);
		}

		if (tp->passive_listener == listener) {
			if (tp->connind_sent && !listener->closing &&
			    listener->rq != NULL && tp->discon_ind_m != NULL)
				tcp_discon_ind_listener(listener, tp, err);

			tcp_passive_unlink(listener, tp);
		}

		ke_spinlock_exit_nospl(&listener->lock);
		tcp_release(listener);
	}

	if (tp->rq != NULL && tp->discon_ind_m != NULL) {
		struct T_discon_ind *di = (typeof(di))tp->discon_ind_m->rptr;
		di->PRIM_type = T_DISCON_IND;
		di->DISCON_reason = err;
		di->SEQ_number = -1;
		tp->discon_ind_m->db->type = M_PROTO;
		tp->discon_ind_m->wptr =
		    tp->discon_ind_m->rptr + sizeof(*di);
		tp->pending_discon = true;
	}

	tcb_free_connstate(tp);

	if (tp->rq == NULL)
		tcp_release(tp);
}

/*
 * reassembly/segment delivery
 */

/* reuse header fields for our own bookkeeping within tcp_conn_input */
#define th_1stdata th_sport
#define th_datalen th_dport

static void
tcp_snd_q_consume(tcp_t *tp, int bytes_acked)
{
	mblk_q_t *msgq;
	uint32_t *countp;
	mblk_t *m, *next;

	kassert(ke_spinlock_held(&tp->lock));

	if (tcp_wq(tp) != NULL) {
		msgq = &tcp_wq(tp)->msgq;
		countp = &tcp_wq(tp)->count;
	} else {
		msgq = &tp->detached_snd_q;
		countp = &tp->detached_snd_q_count;
	}

	if (bytes_acked >= (int)*countp) {
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

		if (bytes_acked >= (int)size) {
			bytes_acked -= size;
			*countp -= size;
			TAILQ_REMOVE(msgq, m, link);
			str_freeb(m);
		} else {
			m->rptr += bytes_acked;
			*countp -= bytes_acked;
			return;
		}

		m = next;
	}
}

static struct tcphdr *
mtoth(mblk_t *m)
{
	return (struct tcphdr *)m->rptr;
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

	TAILQ_FOREACH(mq, &tp->reass_queue, link) {
		thq = mtoth(mq);
		if (SEQ_GT(ntohl(thq->th_seq), seg_seq))
			break;
	}

	prev = NULL;
	if (mq != NULL)
		prev = TAILQ_PREV(mq, msgb_q, link);
	else
		prev = TAILQ_LAST(&tp->reass_queue, msgb_q);

	if (prev != NULL) {
		struct tcphdr *thprev = mtoth(prev);
		tcp_seq_t prev_seq = ntohl(thprev->th_seq);
		tcp_seq_t prev_end = prev_seq + SEG_LEN(thprev);

		if (SEQ_GT(prev_end, seg_seq)) {
			uint32_t overlap = prev_end - seg_seq;
			uint32_t datatrim = MIN2(overlap, th->th_datalen);

			if (overlap >= SEG_LEN(th)) {
				TCP_TRACE("Segment completely overlapped, "
				    "dropping\n");
				return false;
			}

			th->th_seq = htonl(seg_seq + datatrim);
			th->th_1stdata += datatrim;
			th->th_datalen -= datatrim;
			seg_seq += datatrim;

			if (overlap > datatrim)
				th->th_flags &= ~TH_FIN;

			seg_end = seg_seq + SEG_LEN(th);
		}
	}

	*drop_mp = false;

	if (mq != NULL)
		TAILQ_INSERT_BEFORE(mq, m, link);
	else
		TAILQ_INSERT_TAIL(&tp->reass_queue, m, link);

	mq = TAILQ_NEXT(m, link);
	while (mq != NULL) {
		tcp_seq_t succ_seq;

		thq = mtoth(mq);
		succ_seq = ntohl(thq->th_seq);

		if (SEQ_GT(seg_end, succ_seq)) {
			uint32_t overlap = seg_end - succ_seq;

			if (overlap >= SEG_LEN(thq)) {
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
			}
		} else {
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

		tp->rcv_nxt += thq->th_datalen;

		if (thq->th_flags & TH_FIN) {
			tp->rcv_nxt++;
			fin_reached = true;
		}

		next = TAILQ_NEXT(mq, link);
		TAILQ_REMOVE(&tp->reass_queue, mq, link);

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
		 * Always queue into rcv_q; tcp_rsrv drains upstream.
		 * Shrink the advertised window as data accumulates.
		 */
		TAILQ_INSERT_TAIL(&tp->rcv_q, data_head, link);
		tp->rcv_q_count += str_msgsize(data_head);
		tp->rcv_wnd -= str_msgsize(data_head);
	}

	return fin_reached;
#undef SEG_LEN
}

static void
tcp_rtt_update(tcp_t *tp, tcp_seq_t ack_seq)
{
	kabstime_t now = ke_time();
	uint32_t r;

	if (!tp->timing_rtt || !SEQ_GEQ(ack_seq, tp->rtseq))
		return;

	r = (uint32_t)((now - tp->rtstart) / 1000000ULL);

	TCP_TRACE("Measured RTT = %u ms for seq %u\n", r, tp->rtseq);

#define ALPHA_RTT_SHIFT   3
#define BETA_RTTVAR_SHIFT 2
#define K_RTO_MULTIPLIER  4

	if (tp->srtt == 0) {
		tp->srtt = r;
		tp->rttvar = r >> 1;
	} else {
		uint32_t delta = (tp->srtt > r) ? (tp->srtt - r) :
						  (r - tp->srtt);

		tp->rttvar = tp->rttvar - (tp->rttvar >> BETA_RTTVAR_SHIFT) +
		    (delta >> BETA_RTTVAR_SHIFT);

		tp->srtt = tp->srtt - (tp->srtt >> ALPHA_RTT_SHIFT) +
		    (r >> ALPHA_RTT_SHIFT);
	}

	tp->rto = tp->srtt + MAX2(1, K_RTO_MULTIPLIER * tp->rttvar);
	tp->rto = MIN2(MAX2(tp->rto, TCP_MIN_RTO_MS), TCP_MAX_RTO_MS);

	tp->timing_rtt = false;
	tp->n_rexmits = 0;
}

static tcp_t *
tcp_passive_open(tcp_t *listener, const struct ip *ip, const struct tcphdr *th,
    in_port_t sport, in_port_t dport)
{
	tcp_t *child;
	struct sockaddr_in faddr;
	union sockaddr_union rt_dst = {
		.in = { .sin_family = AF_INET, .sin_addr = ip->ip_src }
	};
	route_result_t rt;
	ip_ifaddr_t *ifa;
	struct in_addr laddr = { .s_addr = INADDR_ANY };
	uint32_t mtu;
	uint16_t mss;
	int r;

	r = route_lookup(&rt_dst, &rt, true);
	if (r != 0)
		return NULL;

	RCULIST_FOREACH(ifa, &rt.ifp->addrs, rlentry) {
		if (ifa->addr.sa.sa_family == AF_INET) {
			laddr = ifa->addr.in.sin_addr;
			break;
		}
	}

#if 0 /* when we have PMTUD */
	mtu = rt.mtu != 0 ? rt.mtu : TCP_DEFAULT_MTU;
	mss = mtu - sizeof(struct ether_header) - sizeof(struct ip) -
	    sizeof(struct tcphdr);
#endif
	mss = TCP_MSS;
	ip_if_release(rt.ifp);

	child = tcp_new(NULL);
	if (child == NULL)
		return NULL;

	ke_spinlock_enter_nospl(&child->lock);

	child->mss = mss;

	child->laddr.sin_family = AF_INET;
	child->laddr.sin_port = dport;
	child->laddr.sin_addr = laddr;

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

	ke_spinlock_enter_nospl(&tcp_conntab_lock);
	r = tcp_conn_table_insert_locked(child);
	ke_spinlock_exit_nospl(&tcp_conntab_lock);
	if (r != 0)
		goto fail;

	child->irs = ntohl(th->th_seq);
	child->rcv_nxt = child->irs + 1;
	child->snd_wnd = ntohs(th->th_win);
	child->snd_wl1 = ntohl(th->th_seq);
	child->snd_wl2 = 0;

	child->passive_listener = listener;
	tcp_retain(listener);

	child->connind_seq = listener->next_connind_seq++;
	LIST_INSERT_HEAD(&listener->conninds, child, conninds_entry);
	tcp_retain(child);

	tcp_change_state(child, TCPS_SYN_RECEIVED);
	return child;

fail:
	ke_spinlock_exit_nospl(&child->lock);
	tcb_free_connstate(child);
	tcp_release(child);
	return NULL;
}

static void
tcp_cc_on_dup_ack(tcp_t *tp)
{
	if (tp->dupacks != UINT8_MAX)
		tp->dupacks++;

	if (tp->fast_recovery) {
		tp->snd_cwnd += tp->mss;
		tcp_output(tp);
		return;
	}

	if (tp->dupacks != 3)
		return;

	if (!SEQ_GT(tp->snd_una, tp->snd_recover))
		return;

	tp->snd_ssthresh = MAX2((tp->snd_max - tp->snd_una) / 2,
	    2 * (uint32_t)tp->mss);
	tp->snd_recover = tp->snd_max;
	tp->snd_cwnd = tp->snd_ssthresh + 3 * tp->mss;
	tp->fast_recovery = true;

	tp->timing_rtt = false;

	tcp_rexmit(tp);
}

static void
tcp_cc_on_new_ack(tcp_t *tp, uint32_t acked_data, tcp_seq_t ack)
{
	tp->dupacks = 0;
	tp->n_rexmits = 0;

	if (tp->fast_recovery) {
		if (SEQ_GEQ(ack, tp->snd_recover)) {
			tp->snd_cwnd = tp->snd_ssthresh;
			tp->fast_recovery = false;
			return;
		} else {
			tp->snd_cwnd -= MIN2(tp->snd_cwnd, acked_data);
			if (acked_data >= tp->mss)
				tp->snd_cwnd += tp->mss;
			tp->snd_cwnd = MAX2(tp->snd_cwnd, (uint32_t)tp->mss);

			tp->timing_rtt = false;
			tcp_rexmit(tp);
			tcp_output(tp);
			return;
		}
	}

	if (tp->snd_cwnd < tp->snd_ssthresh) {
		tp->snd_cwnd += MIN2((uint32_t)acked_data, tp->mss);
	} else {
		tp->bytes_acked += acked_data;
		if (tp->bytes_acked >= tp->snd_cwnd) {
			tp->bytes_acked -= tp->snd_cwnd;
			tp->snd_cwnd += tp->mss;
		}
	}
}

void
tcp_conn_input(tcp_t **tpp, mblk_t *mp, const ip_rxattr_t *attr)
{
	const struct ip *ip = attr->l3hdr.ip4;
	uint16_t tcp_len = (uint16_t)(mp->wptr - mp->rptr);
	struct tcphdr *th = (struct tcphdr *)mp->rptr;
	bool got_fin = false;
	uint16_t sport, dport;
	bool dropmp = true;
	tcp_t *tp = *tpp;

	kassert(ke_spinlock_held(&tp->lock));

	sport = th->th_sport;
	dport = th->th_dport;

#define TCP_REPLY_RESET() \
do { \
	th->th_sport = sport; \
	th->th_dport = dport; \
	tcp_reply_reset(ip, th, tcp_len); \
	goto finish; \
} while (0)

	th->th_1stdata = (th->th_off << 2);
	th->th_datalen = tcp_len - th->th_1stdata;

	/* 3.10.7: SEGMENT ARRIVES */
	switch (tp->state) {
	case TCPS_CLOSED:
		TCP_REPLY_RESET();

	case TCPS_LISTEN:
		if (th->th_flags & TH_RST)
			goto finish;

		if (th->th_flags & TH_ACK)
			TCP_REPLY_RESET();

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
			 * Henceforth processing is now on the passive child.
			 * child->lock is held; listener->lock is released.
			 */
			ke_spinlock_exit_nospl(&listener->lock);

			tp = child;
			*tpp = tp;

			th->th_seq = htonl(ntohl(th->th_seq) + 1);

			if (th->th_datalen > 0 &&
			    th->th_datalen > tp->rcv_wnd) {
				th->th_flags &= ~TH_FIN;
				th->th_datalen = MIN2(th->th_datalen,
				    tp->rcv_wnd);
			}

			goto step_6;
		}

		goto finish;

	case TCPS_SYN_SENT:
		if (th->th_flags & TH_ACK) {
			if (SEQ_LEQ(ntohl(th->th_ack), tp->iss) ||
			    SEQ_GT(ntohl(th->th_ack), tp->snd_max))
				TCP_REPLY_RESET();
		}

		if (th->th_flags & TH_RST) {
			if (th->th_flags & TH_ACK) {
				tcp_abort(tp, ECONNRESET);
				goto finish;
			} else {
				goto finish;
			}
		}

		if (th->th_flags & TH_SYN) {
			tp->irs = ntohl(th->th_seq);
			tp->rcv_nxt = tp->irs + 1;

			if (th->th_flags & TH_ACK)
				tp->snd_una = ntohl(th->th_ack);

			if (SEQ_GT(tp->snd_una, tp->iss)) {
				tcp_change_state(tp, TCPS_ESTABLISHED);
				tcp_conn_con(tp, ip, th, sport);
				tp->emit_ack = true;
			} else {
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

		if (!(th->th_flags & (TH_SYN | TH_RST)))
			goto finish;

	default:
		break;
	}

	/* First, check sequence number: */

	if (tp->rcv_wnd == 0) {
		if (ntohl(th->th_seq) != tp->rcv_nxt) {
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
		if (SEQ_LT(ntohl(th->th_seq), tp->rcv_nxt) ||
		    SEQ_GT(ntohl(th->th_seq), tp->rcv_nxt + tp->rcv_wnd)) {
			goto finish;
		} else if (ntohl(th->th_seq) == tp->rcv_nxt) {
			switch (tp->state) {
			case TCPS_SYN_RECEIVED:
				tcp_abort(tp, ECONNRESET);
				goto finish;

			case TCPS_ESTABLISHED:
			case TCPS_FIN_WAIT_1:
			case TCPS_FIN_WAIT_2:
			case TCPS_CLOSE_WAIT:
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
			th->th_sport = sport;
			th->th_dport = dport;
			tcp_reply(ip, th, tp->snd_nxt, tp->rcv_nxt, TH_ACK);
			goto finish;
		}
	}

	/* Fourth, check the SYN bit: */
	if (th->th_flags & TH_SYN) {
		tcp_abort(tp, ECONNRESET);
		TCP_REPLY_RESET();
	}

	/* Fifth, check the ACK field: */
	if (!(th->th_flags & TH_ACK)) {
		if (tp->emit_ack)
			tcp_output(tp);
		goto finish;
	}

	switch (tp->state) {
	case TCPS_SYN_RECEIVED:
		if (SEQ_LT(tp->snd_una, ntohl(th->th_ack)) &&
		    SEQ_LEQ(ntohl(th->th_ack), tp->snd_max)) {
			/* advance for SYN bit */
			tp->snd_una++;

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
			TCP_REPLY_RESET();
		}

	case TCPS_ESTABLISHED:
	case TCPS_FIN_WAIT_1:
	case TCPS_FIN_WAIT_2:
	case TCPS_CLOSE_WAIT:
	case TCPS_CLOSING:
	case TCPS_LAST_ACK:
	case TCPS_TIME_WAIT: {
		bool our_fin_acked = false;

		if (SEQ_LEQ(ntohl(th->th_ack), tp->snd_una)) {
			if (tp->snd_una != tp->snd_max &&
			    th->th_datalen == 0 &&
			    (th->th_flags & (TH_SYN | TH_FIN)) == 0 &&
			    ntohl(th->th_ack) == tp->snd_una &&
			    ntohs(th->th_win) == tp->snd_wnd) {
				tcp_cc_on_dup_ack(tp);
				goto finish;
			}

		} else if (SEQ_GT(ntohl(th->th_ack), tp->snd_max)) {
			tp->emit_ack = true;
			tcp_output(tp);
			goto finish;
		} else {
			uint32_t nseq_acked, bytes_acked;

		acceptable_ack:
			nseq_acked = ntohl(th->th_ack) - tp->snd_una;
			bytes_acked = MIN2(nseq_acked, tcp_snd_q_count(tp));

			tp->snd_una = ntohl(th->th_ack);
			if (SEQ_LT(tp->snd_nxt, tp->snd_una))
				tp->snd_nxt = tp->snd_una;

			if (nseq_acked > tcp_snd_q_count(tp)) {
				tp->snd_wnd -= tcp_snd_q_count(tp);
				tcp_snd_q_consume(tp, tcp_snd_q_count(tp));
				our_fin_acked = true;
			} else {
				tp->snd_wnd -= nseq_acked;
				tcp_snd_q_consume(tp, nseq_acked);
			}

			tcp_cc_on_new_ack(tp, bytes_acked, ntohl(th->th_ack));

			if (tp->snd_una == tp->snd_max)
				tcp_cancel_timer(tp, TCP_TIMER_REXMT);
			else
				tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);

			tcp_rtt_update(tp, ntohl(th->th_ack));
		}

		if (SEQ_LT(tp->snd_wl1, ntohl(th->th_seq)) ||
		    (tp->snd_wl1 == ntohl(th->th_seq) &&
			SEQ_LEQ(tp->snd_wl2, ntohl(th->th_ack)))) {
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
				if (tp->rq == NULL || tp->shutdown_rd)
					tcp_set_timer(tp, TCP_TIMER_2MSL,
					    TCP_2MSL_MS);
			}
			break;

		case TCPS_CLOSING:
			if (our_fin_acked) {
				tcp_change_state(tp, TCPS_TIME_WAIT);
				tcp_cancel_all_timers(tp);
				tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			} else {
				goto finish;
			}
			break;

		case TCPS_LAST_ACK:
			if (our_fin_acked) {
				tcb_free_connstate(tp);
				tcp_release(tp);
				goto finish;
			}
			break;

		case TCPS_TIME_WAIT:
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
	if (th->th_flags & TH_URG)
		TCP_TRACE("TH_URG currently ignored!\n");

	if (th->th_datalen > 0 || (th->th_flags & TH_FIN)) {
		got_fin = tcp_queue_for_reassembly(tp, mp, th, &dropmp);
		tp->emit_ack = true;
	}

	if (got_fin) {
		if (tp->state <= TCPS_SYN_SENT)
			goto finish;

		switch (tp->state) {
		case TCPS_SYN_RECEIVED:
		case TCPS_ESTABLISHED:
			tcp_change_state(tp, TCPS_CLOSE_WAIT);
			tp->ordrel_needed = true;
			break;

		case TCPS_FIN_WAIT_1:
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
			break;

		case TCPS_TIME_WAIT:
			tcp_set_timer(tp, TCP_TIMER_2MSL, TCP_2MSL_MS);
			break;

		default:
			kunreachable();
		}

		if (tp->ordrel_needed && tp->rq != NULL &&
		    TAILQ_EMPTY(&tp->rcv_q))
			tcp_ordrel_ind(tp);
		else if (tp->ordrel_needed)
			tp->pending_ordrel = true;
	}

	tcp_output(tp);

finish:
	if (dropmp)
		str_freemsg(mp);
}

void
tcp_ipv4_input(ip_if_t *ifp, mblk_t *mp, ip_rxattr_t *attr)
{
	const struct ip *ip = attr->l3hdr.ip4;
	struct tcphdr *th;
	uint16_t tcp_len;
	tcp_t *tp, *tp2;
	bool wake;
	queue_t *rq;
	ipl_t ipl;

	(void)ifp;

	tcp_len = (uint16_t)(mp->wptr - mp->rptr);

	if (tcp_len < sizeof(struct tcphdr)) {
		TCP_TRACE("Header too small\n");
		str_freemsg(mp);
		return;
	}

	th = (struct tcphdr *)mp->rptr;

	TCP_TRACE(" -- src=" FMT_IP4 ":%u dst=" FMT_IP4 ":%u "
	    "seq=%u ack=%u len=%u flags=%s%s%s%s%s%s\n",
	    ARG_IP4(ip->ip_src.s_addr), ntohs(th->th_sport),
	    ARG_IP4(ip->ip_dst.s_addr), ntohs(th->th_dport),
	    ntohl(th->th_seq), ntohl(th->th_ack), tcp_len,
	    (th->th_flags & TH_FIN) ? "FIN " : "",
	    (th->th_flags & TH_SYN) ? "SYN " : "",
	    (th->th_flags & TH_RST) ? "RST " : "",
	    (th->th_flags & TH_PUSH) ? "PSH " : "",
	    (th->th_flags & TH_ACK) ? "ACK " : "",
	    (th->th_flags & TH_URG) ? "URG " : "");

	ipl = ke_spinlock_enter(&tcp_conntab_lock);
	tp = tcp_conn_lookup_locked(ip->ip_src, th->th_sport, ip->ip_dst,
	    th->th_dport);
	if (tp != NULL)
		tcp_retain(tp);
	ke_spinlock_exit(&tcp_conntab_lock, ipl);

	if (tp == NULL) {
		TCP_TRACE("No matching connection found\n");
		tcp_reply_reset(ip, th, tcp_len);
		str_freemsg(mp);
		return;
	}

	tp2 = tp;

	ipl = ke_spinlock_enter(&tp->lock);
	tcp_conn_input(&tp2, mp, attr);

	wake = tp2->rq != NULL &&
	    (!TAILQ_EMPTY(&tp2->rcv_q) || tp2->pending_conn_con ||
		tp2->pending_ordrel || tp2->pending_discon);
	rq = tp2->rq;

	ke_spinlock_exit(&tp2->lock, ipl);

	if (wake)
		str_qenable(rq);

	tcp_release(tp);
}

/*
 * Timer infrastructure
 */

void
tcp_set_timer(tcp_t *tp, enum tcp_timer_type type, uint32_t timeout_ms)
{
	kabstime_t deadline = ke_time() + (kabstime_t)timeout_ms * NS_PER_MS;
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

	ipl = ke_spinlock_enter(&tp->lock);

	if (tp->rq != NULL) {
		str_qenable(tcp_wq(tp));
		str_kick(tp->rq->stdata);
		ke_spinlock_exit(&tp->lock, ipl);
		tcp_release(tp);
	} else {
		uint32_t pending = atomic_exchange(&tp->pending_timers, 0);
		tcp_dispatch_pending_timers(tp, pending);
		ke_spinlock_exit(&tp->lock, ipl);
		tcp_release(tp);
	}
}

static void
tcp_rexmt_timer(tcp_t *tp)
{
	TCP_TRACE("TCP retransmit timer expired\n");

	if (tp->timers[TCP_TIMER_REXMT].deadline > ke_time())
		return;

	if (tp->n_rexmits >= TCP_MAXRETRIES) {
		TCP_TRACE("Too many rexmit attempts, giving up\n");
		tcp_abort(tp, ETIMEDOUT);
		return;
	}

	if (tp->n_rexmits == 0) {
		tp->snd_ssthresh = MAX2((tp->snd_max - tp->snd_una) / 2,
		    2 * (uint32_t)tp->mss);
		tp->snd_recover = tp->snd_max;
	}

	tp->n_rexmits++;
	tp->snd_cwnd = tp->mss;	/* restart slow start */
	tp->fast_recovery = false;
	tp->dupacks = 0;

	/*
	 * Retransmit timer expired - stop timing RTT, backoff RTO, and let the
	 * next sequence to be sent be the first unacknowledged one.
	*/

	tp->timing_rtt = false;
	tp->rto = MIN2(tp->rto * 2, TCP_MAX_RTO_MS);
	tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);

	/* snd_cwnd == mss by here */
	tcp_rexmit(tp);
}

static void
tcp_persist_timer(tcp_t *tp)
{
	(void)tp;
	TCP_TRACE("TODO: TCP persist timer expired\n");
}

static void
tcp_keepalive_timer(tcp_t *tp)
{
	(void)tp;
	TCP_TRACE("TODO: TCP keepalive timer expired\n");
}

static void
tcp_2msl_timer(tcp_t *tp)
{
	TCP_TRACE("TCP 2MSL timer expired\n");

	if (tp->timers[TCP_TIMER_2MSL].deadline > ke_time())
		return;

	kassert(tp->state == TCPS_FIN_WAIT_2 || tp->state == TCPS_TIME_WAIT);

	tcb_free_connstate(tp);

	/*
	 * If detached, then we own the last reference to the TCP, and are
	 * responsible to free it.
	 */
	if (tp->rq == NULL)
		tcp_release(tp);
}

void
tcp_init(void)
{
	ke_spinlock_init(&tcp_conntab_lock);
}
