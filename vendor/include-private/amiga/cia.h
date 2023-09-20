/*
 * Mach Operating System
 * Copyright (c) 1992 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/* This is a rewrite (retype) of the Amiga's CIA chip register map, based
   on the Hardware Reference Manual.  It is NOT based on the Amiga's
   hardware/cia.h.  */

#ifndef _amiga_cia_
#define _amiga_cia_

#define PHYS_CIAA 0xbfe001
#define PHYS_CIAB 0xbfd000

/* timer A */

#define CIAICRF_TA (1 << 0)
#define CIAICRF_SETCLR (1 << 7)

#define CIACRAF_START (1 << 0)
#define CIACRAF_SPMODE (1 << 6)
#define CIACRAF_TODIN (1 << 7)

struct CIA
  {
    unsigned char pra;          char pad0[0xff];
    unsigned char prb;          char pad1[0xff];
    unsigned char ddra;         char pad2[0xff];
    unsigned char ddrb;         char pad3[0xff];
    unsigned char talo;         char pad4[0xff];
    unsigned char tahi;         char pad5[0xff];
    unsigned char tblo;         char pad6[0xff];
    unsigned char tbhi;         char pad7[0xff];
    unsigned char todlo;        char pad8[0xff];
    unsigned char todmid;       char pad9[0xff];
    unsigned char todhi;        char pada[0x1ff];
    unsigned char sdr;          char padc[0xff];
    unsigned char icr;          char padd[0xff];
    unsigned char cra;          char pade[0xff];
    unsigned char crb;          char padf[0xff];
  };

#define ciaa (*((volatile struct CIA *)PHYS_CIAA))
#define ciab (*((volatile struct CIA *)PHYS_CIAB))

#endif /* _amiga_cia_ */
