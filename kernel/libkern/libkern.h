#ifndef LIBKERN_H_
#define LIBKERN_H_

#include <nanokern/kernmisc.h>

#define kprintf nk_dbg
#define kfatal nk_fatal
#define kassert nk_assert

struct msgbuf {
	char buf[4096];
	size_t read, write;
};

extern struct msgbuf msgbuf;

#endif /* LIBKERN_H_ */
