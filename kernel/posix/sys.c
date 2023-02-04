#include <libkern/libkern.h>
#include <md/intr.h>
#include <posix/proc.h>
#include <posix/sys.h>

int
posix_syscall(md_intr_frame_t *frame)
{
	proc_t *proc = curthread()->process->psxproc;
	// kthread_t *thread = curthread();
	uintptr_t err = 0;

#define ARG1 frame->rdi
#define ARG2 frame->rsi
#define ARG3 frame->rdx
#define ARG4 frame->r10
#define ARG5 frame->r8
#define ARG6 frame->r9

#define RET frame->rax
#define ERR frame->rdi

	switch (frame->rax) {
	case kPXSysDebug: {
		nk_dbg("SYS_POSIX: %s\n", (char *)ARG1);
		md_intr_frame_trace(frame);
		break;
	}

	case kPXSysExecVE: {
		int r = sys_exec(proc, (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);

		if (r < 0) {
			RET = -1;
			err = -r;
		}

		break;
	}

	default: {
		nk_fatal("unhandled syscall number %lu\n", frame->rax);
	}
	}

	ERR = err;
	return 0;
}
