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
#include <sys/stream.h>
#include <sys/tihdr.h>

#include <netinet/in.h>

#include <inet/ip.h>
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

typedef struct tcp_endp {
	kmutex_t	mutex;
	atomic_uint	refcnt;

	kspinlock_t	detach_lock;	/* involved in detaching */
	queue_t		*rq;		/* stream read queue, if not detached */

	/* in detached state, our send queue */
	mblk_q_t detached_snd_q;
	size_t detached_snd_q_count;

	/* in detached state, our receive queue */
	/* TODO could merge with above? detached close = hard reset on recv */
	mblk_q_t detached_rcv_q;
	size_t detached_rcv_q_count;

	enum tcp_state state;	/* state of TCB */
	int conn_id;		/* ID in the connections/binds table */

	struct sockaddr_in laddr;	/* local address */
	struct sockaddr_in faddr;	/* foreign address */

	struct {
		kcallout_t	callout;
		kdpc_t		dpc;
		kabstime_t	deadline;
	} timers[TCP_TIMER_MAX]; /* TCP timers */

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
} tcp_endp_t;

static int tcp_open(queue_t *, void *dev);
static void tcp_close(queue_t *);
static void tcp_wput(queue_t *, mblk_t *);
static void tcp_rput(queue_t *, mblk_t *);
static void tcp_rsrv(queue_t *);
static void tcp_timer_dpchandler(void *, void *);

struct qinit tcp_rinit = {
	.qopen = tcp_open,
	.qclose = tcp_close,
	.putp = tcp_rput,
	.srvp = tcp_rsrv,
};

static struct qinit tcp_winit = {
	.putp = tcp_wput,
};

struct streamtab tcp_streamtab = {
	.rinit = &tcp_rinit,
	.winit = &tcp_winit,
};


static tcp_endp_t *
tcb_new(queue_t *rq)
{
	tcp_endp_t *tp = kmem_alloc(sizeof(*tp));
	if (tp == NULL)
		return NULL;

	ke_mutex_init(&tp->mutex);
	tp->refcnt = 1;

	tp->state = TCPS_CLOSED;
	tp->conn_id = -1;

	ke_spinlock_init(&tp->detach_lock);

	tp->rq = rq;

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

	TAILQ_INIT(&tp->reass_queue);

	return tp;
}

static int
tcp_open(queue_t *rq, void *)
{
	tcp_endp_t *tp = tcb_new(rq);
	if (tp == NULL)
		return -ENOMEM;
	rq->ptr = tp;
	return 0;
}

static void
tcp_close(queue_t *rq)
{
	ktodo();
}

/*
 * output processing
 */

static void
tcp_wput(queue_t *wq, mblk_t *mp)
{
	ktodo();
}

/*
 * input processing
 */

static void
tcp_rput(queue_t *rq, mblk_t *mp)
{
	ktodo();
}

static void
tcp_rsrv(queue_t *rq)
{
	ktodo();
}

static void
tcp_timer_dpchandler(void *, void *)
{
	ktodo();
}
