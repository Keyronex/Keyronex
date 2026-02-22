/*
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 * Created on Sun Dec 21 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file jobctl.c
 * @brief Brief explanation.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/proc.h>

extern kmutex_t proctree_mutex;
extern TAILQ_HEAD(, proc) allproc;

struct session *
session_alloc(pid_t sid, struct proc *leader)
{
	struct session *sess;

	kassert(ke_mutex_held(&proctree_mutex));

	sess = kmem_alloc(sizeof(*sess));
	if (sess == NULL)
		return NULL;

	sess->refcount = 1;
	sess->sid = sid;
	sess->leader = leader;
	sess->ctty_str = NULL;
	sess->ctty_vn = NULL;
	LIST_INIT(&sess->pgrps);

	return sess;
}

void
session_ref(struct session *sess)
{
	kassert(ke_mutex_held(&proctree_mutex));

	sess->refcount++;
}

void
session_unref(struct session *sess)
{
	kassert(ke_mutex_held(&proctree_mutex));

	kassert(sess->refcount > 0);
	if (--sess->refcount == 0) {
		kassert(LIST_EMPTY(&sess->pgrps));
		kmem_free(sess, sizeof(*sess));
	}
}

struct pgrp *
pgrp_alloc(pid_t pgid, struct session *sess)
{
	struct pgrp *pgrp;

	kassert(ke_mutex_held(&proctree_mutex));

	pgrp = kmem_alloc(sizeof(*pgrp));
	if (pgrp == NULL)
		return NULL;

	pgrp->refcount = 1;
	pgrp->pgid = pgid;
	pgrp->session = sess;
	LIST_INIT(&pgrp->members);

	session_ref(sess);
	LIST_INSERT_HEAD(&sess->pgrps, pgrp, session_link);

	return pgrp;
}

void
pgrp_ref(struct pgrp *pgrp)
{
	kassert(ke_mutex_held(&proctree_mutex));
	pgrp->refcount++;
}

void
pgrp_unref(struct pgrp *pgrp)
{
	kassert(ke_mutex_held(&proctree_mutex));
	kassert(pgrp->refcount > 0);
	if (--pgrp->refcount == 0) {
		struct session *sess = pgrp->session;

		kassert(LIST_EMPTY(&pgrp->members));
		LIST_REMOVE(pgrp, session_link);
		kmem_free(pgrp, sizeof(*pgrp));

		session_unref(sess);
	}
}

void
pgrp_add_member(struct pgrp *pgrp, struct proc *proc)
{
	kassert(ke_mutex_held(&proctree_mutex));

	if (proc->pgrp != NULL)
		pgrp_remove_member(proc);

	pgrp_ref(pgrp);
	proc->pgrp = pgrp;
	LIST_INSERT_HEAD(&pgrp->members, proc, pgrp_link);
}

void
pgrp_remove_member(struct proc *proc)
{
	struct pgrp *pgrp = proc->pgrp;

	kassert(ke_mutex_held(&proctree_mutex));

	if (pgrp == NULL)
		return;

	LIST_REMOVE(proc, pgrp_link);
	proc->pgrp = NULL;
	pgrp_unref(pgrp);
}

pid_t
sys_getpgid(pid_t pid)
{
	proc_t *proc, *target;
	pid_t pgid;

	proc = curproc();

	ke_mutex_enter(&proctree_mutex, "sys_getpgid");

	if (pid == 0) {
		target = proc;
	} else {
		target = NULL;
		TAILQ_FOREACH(target, &allproc, allproc_qlink) {
			if (target->pid == pid)
				break;
		}
		if (target == NULL) {
			ke_mutex_exit(&proctree_mutex);
			return -ESRCH;
		}
	}

	pgid = target->pgrp ? target->pgrp->pgid : -1;
	ke_mutex_exit(&proctree_mutex);

	return pgid;
}

pid_t
sys_getsid(pid_t pid)
{
	proc_t *proc, *target;
	pid_t sid;

	proc = curproc();

	ke_mutex_enter(&proctree_mutex, "sys_getsid");

	if (pid == 0) {
		target = proc;
	} else {
		target = NULL;
		TAILQ_FOREACH(target, &allproc, allproc_qlink) {
			if (target->pid == pid)
				break;
		}
		if (target == NULL) {
			ke_mutex_exit(&proctree_mutex);
			return -ESRCH;
		}
	}

	sid = target->pgrp ? target->pgrp->session->sid : -1;
	ke_mutex_exit(&proctree_mutex);

	return sid;
}

int
sys_setpgid(pid_t pid, pid_t pgid)
{
	proc_t *proc, *target;
	struct pgrp *pgrp;
	struct session *sess;
	int ret = 0;

	proc = curproc();

	ke_mutex_enter(&proctree_mutex, "sys_setpgid");

	if (pid == 0) {
		target = proc;
	} else {
		target = NULL;
		TAILQ_FOREACH(target, &allproc, allproc_qlink) {
			if (target->pid == pid)
				break;
		}
		if (target == NULL) {
			ke_mutex_exit(&proctree_mutex);
			return -ESRCH;
		}

		if (target != proc && target->parent != proc) {
			/* if neither self nor child, not allowed */
			ke_mutex_exit(&proctree_mutex);
			return -ESRCH;
		}
	}

	if (pgid == 0)
		pgid = target->pid;

	if (target->pgrp && target->pgrp->session->leader == target) {
		/* cannot change pgid of session leader */
		ke_mutex_exit(&proctree_mutex);
		return -EPERM;
	}

	sess = target->pgrp ? target->pgrp->session : NULL;
	if (sess == NULL) {
		/* target has no process group/session somehow */
		ke_mutex_exit(&proctree_mutex);
		return -ESRCH;
	}

	if (target->pgrp && target->pgrp->pgid == pgid) {
		/* already in the right process group */
		ke_mutex_exit(&proctree_mutex);
		return 0;
	}

	pgrp = NULL;
	LIST_FOREACH(pgrp, &sess->pgrps, session_link) {
		if (pgrp->pgid == pgid)
			break;
	}

	if (pgrp == NULL) {
		pgrp = pgrp_alloc(pgid, sess);
		if (pgrp == NULL) {
			ke_mutex_exit(&proctree_mutex);
			return -ENOMEM;
		}
	}

	pgrp_add_member(pgrp, target);
	ke_mutex_exit(&proctree_mutex);
	return ret;
}

pid_t
sys_setsid(void)
{
	proc_t *proc = curproc();
	struct session *sess;
	struct pgrp *pgrp;
	pid_t pid;

	ke_mutex_enter(&proctree_mutex, "sys_setsid");

	pid = proc->pid;

	if (proc->pgrp && proc->pgrp->pgid == pid) {
		/* not allowed, if already a process group leader */
		ke_mutex_exit(&proctree_mutex);
		return -EPERM;
	}

	sess = session_alloc(pid, proc);
	if (sess == NULL) {
		ke_mutex_exit(&proctree_mutex);
		return -ENOMEM;
	}

	pgrp = pgrp_alloc(pid, sess);
	if (pgrp == NULL) {
		session_unref(sess);
		ke_mutex_exit(&proctree_mutex);
		return -ENOMEM;
	}

	pgrp_add_member(pgrp, proc);

	ke_mutex_exit(&proctree_mutex);

	return pid;
}
