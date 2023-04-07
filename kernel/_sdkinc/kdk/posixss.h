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

	kPXSysIOCtl,
	kPXSysOpen,
	kPXSysClose,
	kPXSysRead,
	kPXSysReadLink,
	kPXSysWrite,
	kPXSysSeek,
	kPXSysPPoll,
	kPXSysIsATTY,
	kPXSysReadDir,
	kPXSysStat,
	kPXSysUnlinkAt,

	kPXSysSetFSBase,
	kPXSysExecVE,
	kPXSysExit,
	kPXSysFork,
	kPXSysWaitPID,
	kPXSysGetPID,
	kPXSysGetPPID,
};

enum posix_stat_kind {
	kPXStatKindFD,
	kPXStatKindAt,
	kPXStatKindCWD,
};

#define TCGETS		0x5401
#define TCSETS		0x5402
#define TCSETSW		0x5403
#define TCSETSF		0x5404

#endif /* KRX_POSIX_POSIXSS_H */
