/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file signal.c
 * @brief Signals
 *
 * Currently a stub implementation that only carries out the default action
 * (minus stops.)
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/k_wait.h>
#include <sys/krx_signal.h>
#include <sys/proc.h>
#include <sys/wait.h>

extern kmutex_t proctree_mutex;
extern TAILQ_HEAD(, proc) allproc;

enum sig_default {
	SIG_DEFAULT_TERMINATE,
	SIG_DEFAULT_IGNORE,
	SIG_DEFAULT_STOP,
	SIG_DEFAULT_CONTINUE,
};

static enum sig_default
sig_default_action(int sig)
{
	switch (sig) {
	case SIGCHLD:
	case SIGURG:
	case SIGWINCH:
		return SIG_DEFAULT_IGNORE;

	case SIGCONT:
		return SIG_DEFAULT_CONTINUE;

	case SIGSTOP:
	case SIGTSTP:
	case SIGTTIN:
	case SIGTTOU:
		return SIG_DEFAULT_STOP;

	default:
		return SIG_DEFAULT_TERMINATE;
	}
}

static bool
sig_valid(int sig)
{
	return sig >= 0 && sig <= SIGRTMAX;
}

int
proc_signal_locked(proc_t *proc, int sig)
{
	kassert(ke_mutex_held(&proctree_mutex));

	if (sig == 0)
		return 0;

	switch (sig_default_action(sig)) {
	case SIG_DEFAULT_IGNORE:
	case SIG_DEFAULT_CONTINUE:
		return 0;

	case SIG_DEFAULT_STOP:
		return -ENOSYS;

	case SIG_DEFAULT_TERMINATE:
		/* ignore init until there are proper signals */
		if (proc == initproc || proc->exited)
			return 0;
		proc_terminate(proc, W_EXITCODE(0, sig));
		return 0;
	}

	kunreachable();
}

int
sys_kill(pid_t pid, int sig)
{
	proc_t *proc = curproc(), *target;
	int r = 0;

	if (!sig_valid(sig))
		return -EINVAL;

	ke_mutex_enter(&proctree_mutex, "sys_kill");

	if (pid > 0) {
		target = proc_find_locked(pid);
		r = target != NULL ? proc_signal_locked(target, sig) : -ESRCH;
	} else {
		pid_t pgid = pid == 0 ? proc->pgrp->pgid : -pid;
		bool found = false;

		TAILQ_FOREACH(target, &allproc, allproc_qlink) {
			if (target->pgrp == NULL)
				continue; /* either kernel or already exited */
			if (pid == -1) {
				if (target == initproc || target == proc)
					continue;
			} else if (target->pgrp->pgid != pgid) {
				continue;
			}

			found = true;
			r = proc_signal_locked(target, sig);
			if (r != 0)
				break;
		}

		if (!found)
			r = -ESRCH;
	}

	ke_mutex_exit(&proctree_mutex);

	return r;
}

int
sys_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
	return -ENOSYS;
}

int
sys_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
	return -ENOSYS;
}
