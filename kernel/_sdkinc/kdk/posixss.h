/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 02 2023.
 */

#ifndef KRX_POSIX_POSIXSS_H
#define KRX_POSIX_POSIXSS_H

#ifdef __amd64
#include "./amd64/sysamd64.h"
#else
#error "Port Portable Applications Subsystem to this platform"
#endif

enum posix_syscall {
	/*! debug print */
	kPXSysDebug,
	kPXSysMmap,

	kPXSysOpen,
	kPXSysClose,
	kPXSysRead,
	kPXSysWrite,
	kPXSysSeek,
	kPXSysPPoll,
	kPXSysIsATTY,
	kPXSysReadDir,
	kPXSysStat,

	kPXSysSetFSBase,
	kPXSysExecVE,
	kPXSysExit,
	kPXSysFork,
	kPXSysWaitPID,
	kPXSysGetPID,
	kPXSysGetPPID,
};

#endif /* KRX_POSIX_POSIXSS_H */
