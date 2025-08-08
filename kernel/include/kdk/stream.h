/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Fri Feb 21 2025.
 */
/*!
 * @file stream.h
 * @brief STREAMS definitions.
 */

#ifndef KRX_KDK_STREAM_H
#define KRX_KDK_STREAM_H

#include <kdk/queue.h>
#include <kdk/vm.h>

#include <stdint.h>

typedef struct free_rtn {
	void (*free_func)(char *, size_t, void *);
	void *free_arg;
} frtn_t;

enum datab_type {
	M_DATA,		/* data */
	M_PROTO,	/* protocol */
};

typedef struct datab {
	frtn_t		*db_frtn;	/* free routine */
	char		*db_base;	/* base of data */
	char 		*db_lim;	/* limit of data */
	uint32_t	db_refcnt;	/* reference count */
	enum datab_type	db_type: 8;	/* type of block */
} dblk_t;

typedef struct msgb {
	TAILQ_ENTRY(msgb) b_tqlink;	/* next mblk on queue */
	SLIST_ENTRY(msgb) b_next;	/* next block in chain */
	union {
		struct {
			dblk_t	*b_datap;	/* data block */
			char	*b_rptr;	/* 1st unread byte */
			char	*b_wptr;	/* next byte to be written */
		};
		struct {
			vm_mdl_t *b_mdl;	/* MDL */
			size_t	b_mdl_roff;	/* first unread offset in MDL */
			size_t	b_mdl_woff;	/* next offset to be written */
		};
	};
} mblk_t;

typedef TAILQ_HEAD(msgb_q, msgb) mblk_q_t;

/*! @brief Allocate a message block and buffer. */
mblk_t *allocb(size_t size, int pri);
/*! @brief Allocate a special message block with existing buffer and dtor. */
mblk_t *esballoc(char *buffer, size_t size, int pri, frtn_t *frtn);
/*! @brief Free a single message block - doesn't follow b_cont chain. */
void freeb(mblk_t *mp);
/*! @brief Free a whole message, following the b_cont chain. */
void freemsg(mblk_t *mp);
/*! @brief Link block \p bp to \p mp, if \p mp is null returns \p bp. */
mblk_t *linkmsg(mblk_t *mp, mblk_t *bp);

/*! @brief Get number of data bytes (in linked M_DATA blocks) in a message. */
size_t msgdsize(mblk_t *mp);
/*! @brief Get total number of bytes (in all linked blocks) in a message. */
size_t msgsize(mblk_t *mp);

/*
 * A queue. These are always allocated in pairs.
 */
typedef struct queue {
	struct qinit	*q_qinfo;	/* queue parameters */
	kspinlock_t 	*q_lockp;	/* lock pointer */
	struct stdata 	*q_stream;	/* stream head */
	mblk_q_t	q_q;		/* pending message blocks */
	uint32_t	q_count;	/* count of bytes in queue */
	struct queue	*q_next;	/* next queue in stack */
	union {
		void		*q_ptr;	/* per-queue private pointer */
		uintptr_t	q_data;	/* per-queue private data */
	};
}queue_t ;

queue_t *RD(queue_t *q);
queue_t *WR(queue_t *q);
queue_t *OTHERQ(queue_t *q);

int putnext(queue_t *, mblk_t *);

struct qinit {
	int (*qi_putp)(struct queue *, mblk_t *);
	int (*qi_qopen)(struct queue *);
};

struct streamtab {
	struct qinit *st_rdinit;
	struct qinit *st_wrinit;
};

#endif /* KRX_KDK_STREAM_H */
