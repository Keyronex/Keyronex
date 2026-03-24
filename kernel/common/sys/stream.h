/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file stream.h
 * @brief STREAMS
 */

#ifndef ECX_SYS_STREAM_H
#define ECX_SYS_STREAM_H

#include <sys/k_thread.h>
#include <sys/vnode.h>

struct qinit;
typedef struct queue queue_t;
typedef struct stdata stdata_t;

typedef struct msgb {
	TAILQ_ENTRY(msgb) link;	/* link in msg queue */
	struct msgb	*cont;	/* link in msg chain */
	char 		*rptr;	/* read pointer */
	char 		*wptr;	/* write pointer */
	struct datab 	*db;	/* data block */
} mblk_t;

typedef TAILQ_HEAD(msgb_q, msgb) mblk_q_t;

typedef enum mtype {
	M_DATA,		/* regular data */
	M_PROTO,	/* protocol message */
	M_HANGUP,	/* hangup indication */
	M_IOCTL,	/* ioctl request  */

	/* high-priority follows */

	M_PCPROTO,
	M_SETOPTS,	/* set stream head options */
	M_LINK,		/* link streams */
	M_FLUSH,	/* flush queues */
	M_SIGNAL,	/* send a signal */
	M_IOCACK,	/* ioctl acknowledge */
	M_IOCNAK,	/* ioctl negative acknowledge */
} mtype_t;

typedef struct datab {
	atomic_uint	refcnt;	/* reference count (atomic) */
	mtype_t 	type;	/* data type */
	char		*base;	/* points to first byte */
	char		*lim;	/* points to after last byte */
} dblk_t;

typedef struct queue {
	struct qinit	*qinfo;	/* queue configuration */
	queue_t		*other;	/* other queue of the pair */
	queue_t		*next;	/* next queue in stream */
	queue_t		*back;	/* previous queue in stream */
	stdata_t	*stdata; /* stream head */

	mblk_q_t	msgq;	/* message queue */
	uint32_t	count;	/* count of characters in q */
	uint32_t	hiwat;	/* high watermark */
	uint32_t	lowat;	/* low watermark */

	int wantw:	1,	/* q wants to write (forward flow blocked) */
	    full:	1,	/* q is full (count reached hiwat) */
	    is_readq:	1;	/* is this a read queue? */

	atomic_bool	enabled; /* atomic; q enabled for scheduling */

	void 		*ptr;	/* private data pointer */
} queue_t;

struct qinit {
	void (*putp)(queue_t *, mblk_t *);
	void (*srvp)(queue_t *);
	int (*qopen)(queue_t *, void *dev);
	void (*qclose)(queue_t *);
	uint32_t hiwat;
	uint32_t lowat;
};

struct streamtab {
	struct qinit *rinit;
	struct qinit *winit;
	struct qinit *muxrinit;
	struct qinit *muxwinit;
};

struct linkblk {
	queue_t	*qtop; 	/* upper stream's bottom write q (NULL if I_PLINK) */
	queue_t	*qbot;	/* lower stream's top write q */
	int	index;	/* link index (for I_[P]UNLINK) */

	TAILQ_ENTRY(linkblk) link;
	struct file *lowerfp;	/* lower stream file pointer (retained) */
	struct streamtab *tabtop; /* streamtab of upper stream (if I_PLINK) */
};

enum {
	FLUSHDATA,
	FLUSHALL,
};

mblk_t *str_allocb(size_t);
void str_freeb(mblk_t *);
void str_freemsg(mblk_t *);
size_t str_msgsize(mblk_t *);

#define STR_MBLKHEAD(MP) ((MP)->rptr - (MP)->db->base)

void str_put(queue_t *, mblk_t *);
void str_putnext(queue_t *, mblk_t *);
void str_qreply(queue_t *, mblk_t *);

void str_putq(queue_t *, mblk_t *);
void str_putbq(queue_t *, mblk_t *);
mblk_t *str_getq(queue_t *);

void str_qwait(queue_t *, kevent_t *);

void str_flushq(queue_t *, int flag);

bool str_canput(queue_t *);
bool str_canputnext(queue_t *);

void str_mblk_q_free(mblk_q_t *);

#endif /* ECX_SYS_STREAM_H */
