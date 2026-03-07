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
#include <sys/stream.h>
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

static int tcp_output(tcp_endp_t *);

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

#define TCP_TRACE(...) kdprintf("TCP: " __VA_ARGS__)

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
	rq->ptr = rq->other->ptr = tp;
	return 0;
}

static void
tcp_close(queue_t *rq)
{
	ktodo();
}

static void
tcp_change_state(tcp_endp_t *tp, enum tcp_state newstate)
{
	TCP_TRACE("TCB %p state change %s -> %s\n", tp,
	    tcp_state_names[tp->state], tcp_state_names[newstate]);
	tp->state = newstate;
}


static size_t
tcp_snd_q_count(tcp_endp_t *tp)
{
	if (tp->rq == NULL)
		return tp->detached_snd_q_count;
	else
		return tp->rq->other->count;
}

/*
 * connection/binding managmeent table
 */

#define TCP_CONN_TABLE_SIZE 256
static tcp_endp_t *tcp_conn_table[TCP_CONN_TABLE_SIZE];
static krwlock_t tcp_conntab_lock;

#define TCP_EPHEMERAL_LOW  49152
#define TCP_EPHEMERAL_HIGH 65535
static uint16_t tcp_next_ephemeral = TCP_EPHEMERAL_LOW;
static krwlock_t tcp_bind_lock;

static bool
tcp_port_in_use(uint16_t port, in_addr_t addr)
{
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_endp_t *t = tcp_conn_table[i];
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
tcp_conn_table_insert(tcp_endp_t *tp)
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

#if 0 /* ref it? */
static tcp_endp_t *
tcp_conn_lookup(struct in_addr src, uint16_t sport, struct in_addr dst,
    uint16_t dport)
{
	ke_rwlock_enter_write(&tcp_conntab_lock);
	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcp_endp_t *t = tcp_conn_table[i];
		if (t != NULL && t->laddr.sin_addr.s_addr == dst.s_addr &&
		    t->laddr.sin_port == dport &&
		    t->faddr.sin_addr.s_addr == src.s_addr &&
		    t->faddr.sin_port == sport) {
			spinlock_unlock_nospl(&tcp_conntab_lock);
			return t;
		}
	}

	for (int i = 0; i < TCP_CONN_TABLE_SIZE; i++) {
		tcb_t *t = tcp_conn_table[i];
		if (t != NULL && t->state == TCPS_LISTEN &&
		    t->laddr.sin_port == dport &&
		    (t->laddr.sin_addr.s_addr == INADDR_ANY ||
			t->laddr.sin_addr.s_addr == dst.s_addr)) {
			spinlock_unlock_nospl(&tcp_conntab_lock);
			return t;
		}
	}

	spinlock_unlock_nospl(&tcp_conntab_lock);
	return NULL;
}
#endif

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
tcp_do_bind(tcp_endp_t *tp, struct sockaddr_in *laddr)
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
tcp_setup_connection(tcp_endp_t *tp, struct sockaddr_in *faddr)
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
tcp_wput_conn_req(queue_t *wq, mblk_t *mp)
{
	struct T_conn_req *cr = (struct T_conn_req *)mp->rptr;
	struct sockaddr_in *dest = (struct sockaddr_in *)&cr->DEST;
	tcp_endp_t *tp = wq->ptr;
	int r;

	switch (tp->state) {
	case TCPS_CLOSED: {
		struct sockaddr_in sin = {
			.sin_family = AF_INET,
			.sin_port = 0,
			.sin_addr.s_addr = INADDR_ANY,
		};

		r = tcp_do_bind(tp, &sin);
		if (r != 0) {
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
			reply_error_ack(wq, mp, T_CONN_REQ, -r);
			return;
		}

		tcp_change_state(tp, TCPS_SYN_SENT);
		r = tcp_output(tp);
		if (r != 0) {
			/* TODO: cleanup state? */
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
tcp_wput(queue_t *wq, mblk_t *mp)
{
	switch (mp->db->type) {
	case M_DATA:
		kfatal("M_DATA\n");
		break;

	case M_PROTO: {
		union T_primitives *prim = (union T_primitives *)mp->rptr;
		switch (prim->type) {
		case T_CONN_REQ:
			tcp_wput_conn_req(wq, mp);
			break;

		case T_CONN_RES:
			kfatal("T_CONN_RES");

		case T_BIND_REQ:
			kfatal("T_BIND_REQ");

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

static queue_t *
tcp_wq(tcp_endp_t *tcb)
{
	return tcb->rq != NULL ? tcb->rq->other : NULL;
}

static void
copy_data(tcp_endp_t *tcb, size_t off, size_t len, uint8_t *dst)
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


uint16_t
tcp_checksum(struct ip *ip, struct tcphdr *tcp, int tcp_len)
{
	struct {
		uint32_t src;
		uint32_t dst;
		uint8_t zero;
		uint8_t proto;
		uint16_t tcp_len;
	} __attribute__((packed)) pseudo_hdr;

	uint32_t sum = 0;
	uint16_t *ptr;
	int i;

	pseudo_hdr.src = ip->ip_src.s_addr;
	pseudo_hdr.dst = ip->ip_dst.s_addr;
	pseudo_hdr.zero = 0;
	pseudo_hdr.proto = IPPROTO_TCP;
	pseudo_hdr.tcp_len = htons(tcp_len);

	ptr = (uint16_t *)&pseudo_hdr;
	for (i = 0; i < sizeof(pseudo_hdr) / 2; i++)
		sum += *ptr++;

	ptr = (uint16_t *)tcp;
	for (i = 0; i < tcp_len / 2; i++)
		sum += *ptr++;

	if (tcp_len & 1)
		sum += *((uint8_t *)tcp + tcp_len - 1);

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return ~sum;
}

static int
do_send(tcp_endp_t *tp, uint8_t flags, size_t data_len, size_t data_off)
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

#if 0
	/* if sending anything other than pure ACK,  ensure rexmt timer is on */
	if ((data_len != 0 || (flags & (TH_SYN | TH_FIN))) &&
	    (tp->timers[TCP_TIMER_REXMT].deadline == ABSTIME_NEVER))
		tcp_set_timer(tp, TCP_TIMER_REXMT, tp->rto);
#endif

	return 0;
}

int
tcp_output(tcp_endp_t *tp)
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
	 */
	if (data_len != 0) {
		if (tp->snd_wnd < tp->mss || data_len < tp->mss) {
			if (tp->snd_una != tp->snd_max &&
			    !SEQ_LT(tp->snd_nxt, tp->snd_max))
				data_len = 0;
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
