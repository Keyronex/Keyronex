/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include "kdk/kernel.h"
#include "kdk/machdep.h"
#include "kdk/posixss.h"
#include "posix/pxp.h"

int
posix_syscall(hl_intr_frame_t *frame)
{
#define ARG1 frame->rdi
#define ARG2 frame->rsi
#define ARG3 frame->rdx
#define ARG4 frame->r10
#define ARG5 frame->r8
#define ARG6 frame->r9

#define RET frame->rax
#define OUT frame->rdi

	switch (frame->rax) {
	case kPXSysDebug:
		kdprintf("<DEBUG>: %s\n", (char *)ARG1);
		syscon_printstats();
		break;

	case kPXSysExecVE: {
		RET = sys_exec(px_curproc(), (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);
		break;
	}

	default:
		kfatal("Unknown syscall %lu\n", frame->rax);
	}

	return true;
}