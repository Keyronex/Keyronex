/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Apr 07 2025.
 */
/*!
 * @file sockfs.c
 * @brief Socket filesystem implementation.
 *
 * This implements the BSD sockets API in terms of STREAMS.
 */

#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>

#include <kdk/executive.h>
#include <kdk/file.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/stream.h>
#include <kdk/ti.h>

#include "ios/str_impl.h"

#if 0 /* not ready yet */

/* shut up clang */
#define IOV_MAX 6

#define SO_SCM_RIGHTS (0x4000 + SCM_RIGHTS)
#define SO_SCM_CREDS (0x4000 + SCM_CREDENTIALS)

#define SO_SRC_ADDR (0x5000 + 1)

struct socknode {
	int pf;
	int type;
	stdata_t *stream;
};

static void scm_free(char *, size_t, void *);

static frtn_t scm_frtn = {
	.free_func = scm_free,
	.free_arg = NULL,
};

static void
scm_free(char *buf, size_t len, void *)
{
	union T_primitives *prim = (union T_primitives *)buf;
	char *optbuf;
	size_t optlen;

	switch (prim->type) {
	case T_OPTDATA_REQ:
		optbuf = buf + prim->optdata_req.OPT_offset;
		optlen = prim->optdata_req.OPT_length;
		break;

	case T_UNITDATA_REQ:
		optbuf = buf + prim->unitdata_req.OPT_offset;
		optlen = prim->unitdata_req.OPT_length;
		break;

	default:
		kfatal("Unexpected\n");
	}

	for (struct T_opthdr *opt = TI_OPT_FIRSTHDR(optbuf, optlen); opt != NULL;
	     opt = TI_OPT_NXTHDR(optbuf, optlen, opt)) {
		file_t **files;
		size_t nfiles;

		if (opt->level != SOL_SOCKET || opt->name != SCM_RIGHTS)
			continue;

		files = (file_t **)TI_OPT_DATA(opt);
		nfiles = TI_OPT_DATA_LEN(opt) / sizeof(file_t *);

		for (size_t i = 0; i < nfiles; i++)
			obj_release(files[i]);
	}
}

static int
internalise_fds(eprocess_t *proc, struct T_opthdr *opt, struct cmsghdr *cmsg)
{
	int *fds = (int *)CMSG_DATA(cmsg);
	size_t nfds = cmsg->cmsg_len / sizeof(int);
	file_t **files = (file_t **)TI_OPT_DATA(opt);

	opt->len = TI_OPT_LEN(sizeof(file_t *) * nfds);
	opt->level = SOL_SOCKET;
	opt->name = SCM_RIGHTS;
	opt->status = 0;

	for (size_t i = 0; i < nfds; i++) {
		file_t *file;

		file = ex_object_space_lookup(proc->objspace, fds[i]);
		if (file == NULL)
			return -EBADF;

		files[i] = file;
	}
}

static int
internalise_cmsgs(eprocess_t *proc, const struct msghdr *msg, mblk_t *m)
{
	struct T_opthdr *opt = (struct T_opthdr *)m->b_wptr;

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(msg, cmsg)) {

		switch (cmsg->cmsg_level) {
		case SOL_SOCKET:
			switch (cmsg->cmsg_type) {
			case SCM_RIGHTS: {
				int r = internalise_fds(proc, opt, cmsg);
				if (r != 0)
					return r;
				opt = (struct T_opthdr *)__TI_OPT_NEXT(opt);
				m->b_wptr = (char *)opt;
				break;
			}

			default:
				kprintf("(unhandled SOL_SOCKET cmsg type %d)\n",
				    cmsg->cmsg_type);
				return -EINVAL;
			}

		default:
			kprintf("(unhandled cmsg level %d)\n",
			    cmsg->cmsg_level);
			return -EINVAL;
		}
	}

	return 0;
}

/* Get space required for TI options matching those in msg->msg_control */
static ssize_t
cmsg_optlen(const struct msghdr *msg)
{
	size_t optlen = 0;

	for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg != NULL;
	     cmsg = CMSG_NXTHDR(msg, cmsg)) {

		switch (cmsg->cmsg_level) {
		case SOL_SOCKET:
			switch (cmsg->cmsg_type) {
			case SCM_RIGHTS: {
				size_t nfds;

				if (cmsg->cmsg_len % sizeof(int) != 0)
					return -EINVAL;

				nfds = cmsg->cmsg_len / sizeof(int);
				optlen += TI_OPT_SPACE(sizeof(file_t *) * nfds);

				break;
			}

			default:
				kprintf("(unhandled SOL_SOCKET cmsg type %d)\n",
				    cmsg->cmsg_type);
				return -EINVAL;
			}

		default:
			kprintf("(unhandled cmsg level %d)\n",
			    cmsg->cmsg_level);
			return -EINVAL;
		}
	}

	return optlen;
}

static ex_size_ret_t
do_sendmsg(eprocess_t *proc, struct socknode *so, const struct msghdr *msg,
    int flags)
{
	mblk_t *m = NULL;
	ex_size_ret_t r = 0;
	ssize_t optlen;

	optlen = cmsg_optlen(msg);
	if (optlen < 0)
		return optlen;

	if (so->type == SOCK_STREAM || so->type == SOCK_SEQPACKET) {
		if (optlen != 0) {
			struct T_optdata_req *tdr;
			char *buf;

			buf = kmem_alloc(sizeof(struct T_optdata_req) + optlen);
			m = esballoc(buf, sizeof(struct T_optdata_req) + optlen,
			    0, &scm_frtn);

			m->b_datap->db_type = M_PROTO;

			tdr = (struct T_optdata_req *)m->b_wptr;
			tdr->PRIM_type = T_OPTDATA_REQ;
			tdr->DATA_flag = 0;
			tdr->OPT_length = optlen;
			tdr->OPT_offset = sizeof(struct T_optdata_req);

			m->b_wptr += sizeof(struct T_optdata_req);
		} else {
			struct T_data_req *tdr;

			m = allocb(sizeof(struct T_data_req), 0);
			if (m == NULL)
				return -ENOMEM;

			m->b_datap->db_type = M_PROTO;

			tdr = (struct T_data_req *)m->b_wptr;
			tdr->PRIM_type = T_DATA_REQ;
			tdr->MORE_flag = 0;

			m->b_wptr += sizeof(struct T_data_req);
		}
	} else /* so->type == SOCK_DGRAM */ {
		struct T_unitdata_req *tudr;
		char *buf;
		size_t dst_space = ROUNDUP(msg->msg_namelen, sizeof(uintptr_t));
		bool include_src = false;

		if (so->pf == PF_UNIX /* && sock is bound... */) {
			include_src = true;
			/* temporary, can actually make this smaller */
			optlen += TI_OPT_SPACE(sizeof(struct sockaddr_un));
		}

		buf = kmem_alloc(sizeof(struct T_unitdata_req) + dst_space +
		    optlen);
		m = esballoc(buf, sizeof(struct T_optdata_req) + dst_space +
		    optlen, 0, &scm_frtn);

		m->b_datap->db_type = M_PROTO;

		tudr = (struct T_unitdata_req *)m->b_wptr;
		tudr->PRIM_type = T_UNITDATA_REQ;
		tudr->DEST_length = msg->msg_namelen;
		tudr->DEST_offset = sizeof(struct T_unitdata_req);
		tudr->OPT_length = optlen;
		tudr->OPT_offset = sizeof(struct T_unitdata_req) + dst_space;

		m->b_wptr += sizeof(struct T_unitdata_req);
		memcpy(m->b_wptr, msg->msg_name, msg->msg_namelen);
		m->b_wptr += dst_space;

		if (include_src) {
			struct T_opthdr *opt = (struct T_opthdr *)m->b_wptr;
			opt->level = SOL_SOCKET;
			opt->name = SO_SRC_ADDR;
			opt->len = TI_OPT_LEN(sizeof(struct sockaddr_un));
			opt->status = 0;
			memcpy(TI_OPT_DATA(opt), "The socket's bound address",
			    sizeof(struct sockaddr_un)); /* temporary */
			m->b_wptr += TI_OPT_SPACE(sizeof(struct sockaddr_un));
		}
	}

	if (msg->msg_control != NULL) {
		r = internalise_cmsgs(proc, msg, m);
		if (r != 0) {
			freeb(m);
			return r;
		}
	}

	for (size_t i = 0; i < msg->msg_iovlen; i++) {
		const char *src = msg->msg_iov[i].iov_base;
		size_t len = msg->msg_iov[i].iov_len;

		while (len > 0) {
			size_t chunk = MIN2(len, UINT16_MAX);
			mblk_t *d = allocb(chunk, 0);
			if (d == NULL) {
				freemsg(m);
				return -ENOMEM;
			}

			memcpy(d->b_wptr, src, chunk);
			d->b_wptr += chunk;
			linkmsg(m, d);

			src += chunk;
			len -= chunk;
			r += chunk;
		}
	}

	return r;
}

static ex_size_ret_t
do_recvmsg(eprocess_t *proc, struct socknode *sock, const struct msghdr *msg,
    int flags)
{
	ipl_t ipl;
	mblk_t *m;
	ssize_t len_req = 0;
	ex_size_ret_t r;

	for (size_t i = 0; i < msg->msg_iovlen; i++)
		len_req += msg->msg_iov[i].iov_len;

	r = str_readlock_thread(sock->stream, &ipl);
	if (r != 0)
		return r;

	while (r < len_req) {
		m = TAILQ_FIRST(&sock->stream->sd_rq->q_q);
		if (m == NULL) {
			if (r == 0) {
				if (msg->msg_flags & MSG_DONTWAIT) {
					str_readunlock(sock->stream, ipl);
					return -EAGAIN;;
				} else {
					ke_spinlock_release(&sock->stream->sd_lock, ipl);
					ke_wait(NULL /* TODO */,
					    "sockfs_recvmsg", 0, 0, -1);
				}
			}
		}
		TAILQ_REMOVE(&sock->stream->sd_rq->q_q, m, b_tqlink);
	}
}

ex_size_ret_t
ex_service_sendmsg(eprocess_t *proc, descnum_t desc, const struct msghdr *u_msg,
    int flags)
{
	struct msghdr msg;
	ex_size_ret_t r;

	r = memcpy_from_user(&msg, u_msg, sizeof(struct msghdr));
	if (r != 0)
		return r;

	if (msg.msg_flags & MSG_OOB || msg.msg_flags & MSG_PEEK ||
	    msg.msg_flags & MSG_WAITALL) {
		/* these aren't supported yet */
		return -ENOTSUP;
	}

	/* for now, these aren't permitted */
	if (msg.msg_iov == NULL || msg.msg_iovlen == 0)
		return -EINVAL;

	if (msg.msg_control != NULL) {
		/* reasonable limit - allows 253 FDs */
		if (msg.msg_controllen > 1024 ||
		    msg.msg_controllen < sizeof(struct cmsghdr))
			return -EINVAL;
		else if (msg.msg_controllen == 0)
			msg.msg_control = NULL;
		else {
			msg.msg_control = kmem_alloc(msg.msg_controllen);
			if (msg.msg_control == NULL) {
				r = -ENOMEM;
				goto out;
			}
			r = memcpy_from_user(msg.msg_control,
			    u_msg->msg_control, msg.msg_controllen);
			if (r != 0)
				goto out_control;
		}
	}

	if (msg.msg_name != NULL) {
		if (msg.msg_namelen > sizeof(struct sockaddr_storage))
			return -EINVAL;
		else if (msg.msg_namelen == 0)
			msg.msg_name = NULL;
		else {
			msg.msg_name = kmem_alloc(msg.msg_namelen);
			if (msg.msg_name == NULL) {
				r = -ENOMEM;
				goto out_control;
			}
			r = memcpy_from_user(msg.msg_name, u_msg->msg_name,
			    msg.msg_namelen);
			if (r != 0)
				goto out_name;
		}
	}

	if (msg.msg_iov != NULL) {
		if (msg.msg_iovlen > UIO_MAXIOV)
			return -EINVAL;
		else if (msg.msg_iovlen == 0)
			msg.msg_iov = NULL;
		else {
			msg.msg_iov = kmem_alloc(msg.msg_iovlen *
			    sizeof(struct iovec));
			if (msg.msg_iov == NULL) {
				r = -ENOMEM;
				goto out_name;
			}
			r = memcpy_from_user(msg.msg_iov, u_msg->msg_iov,
			    msg.msg_iovlen * sizeof(struct iovec));
			if (r != 0)
				goto out_iov;
		}
	}

	r = do_sendmsg(proc, NULL, &msg, flags);

out_iov:
	if (msg.msg_iov != NULL)
		kmem_free(msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec));

out_name:
	if (msg.msg_name != NULL)
		kmem_free(msg.msg_name, msg.msg_namelen);

out_control:
	if (msg.msg_control != NULL)
		kmem_free(msg.msg_control, msg.msg_controllen);

out:
	return r;
}

ex_size_ret_t
ex_service_recvmsg(eprocess_t *proc, descnum_t desc, const struct msghdr *u_msg,
    int flags)
{
	struct msghdr msg;
	ex_size_ret_t r;

	r = memcpy_from_user(&msg, u_msg, sizeof(struct msghdr));
	if (r != 0)
		return r;

	/* for now, these aren't permitted */
	if (msg.msg_iov == NULL || msg.msg_iovlen == 0)
		return -EINVAL;

	if (msg.msg_iov != NULL) {
		if (msg.msg_iovlen > UIO_MAXIOV)
			return -EINVAL;
		else if (msg.msg_iovlen == 0)
			msg.msg_iov = NULL;
		else {
			msg.msg_iov = kmem_alloc(msg.msg_iovlen *
			    sizeof(struct iovec));
			if (msg.msg_iov == NULL) {
				r = -ENOMEM;
				goto out;
			}
			r = memcpy_from_user(msg.msg_iov, u_msg->msg_iov,
			    msg.msg_iovlen * sizeof(struct iovec));
			if (r != 0)
				goto out_iov;
		}
	}

	r = do_recvmsg(proc, NULL, &msg, flags);

out_iov:
	if (msg.msg_iov != NULL)
		kmem_free(msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec));

out:
	return r;

}
#endif
