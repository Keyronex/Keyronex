#ifndef KERNMISC_H_
#define KERNMISC_H_

#include <libkern/nanoprintf.h>

#include <nanokern/thread.h>

#define nk_dbg(...)                                                         \
	{                                                                   \
		/* nk_dbg is usable everywhere, so need SPL high */         \
		ipl_t ipl = nk_spinlock_acquire_at(&nk_dbg_lock, kSPLHigh); \
		npf_pprintf(md_dbg_putc, NULL, __VA_ARGS__);                \
		nk_spinlock_release(&nk_dbg_lock, ipl);                     \
	}

#define nk_fatal(...)                \
	{                            \
		nk_dbg(__VA_ARGS__); \
		while (1) {          \
			asm("hlt");  \
		}                    \
	}

#define nk_assert(...)                                                \
	{                                                             \
		if (!(__VA_ARGS__))                                   \
			nk_fatal("nanokernel assertion failed: %s\n", \
			    #__VA_ARGS__);                            \
	}

/*! machine-dependent kernel debug putchar for kernel debug messages */
void md_dbg_putc(int ch, void *ctx);

/*! debug printf lock, acquired at spl high*/
extern kspinlock_t nk_dbg_lock;

#endif /* KERNMISC_H_ */
