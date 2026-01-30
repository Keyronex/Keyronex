/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file ioctls.h
 * @brief Linux <asm/ioctls.h> stub - TTY ioctl definitions.
 */

#ifndef ECX_ASM_IOCTLS_H
#define ECX_ASM_IOCTLS_H

#define TCGETS		0x5401
#define TCSETS		0x5402
#define TCSETSW		0x5403
#define TCSETSF		0x5404
#define TCGETA		0x5405
#define TCSETA		0x5406
#define TCSETAW		0x5407
#define TCSETAF		0x5408

#define TIOCSCTTY	0x540E

#define TIOCGPTN	0x80045430
#define TIOCSPTLCK	0x40045431

#endif /* ECX_ASM_IOCTLS_H */
