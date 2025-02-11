#define NANOPRINTF_IMPLEMENTATION
#include "kdk/nanoprintf.h"
/* the above must come first */

#include "dev/FBTerminal.h"

struct kmsgbuf {
	char buf[4096];
	size_t read, write;
} kmsgbuf;

void
ki_replay_msgbuf(void)
{
#if 0
	for (size_t i = kmsgbuf.read; i != kmsgbuf.write; i++) {
		fbterminal_putc(system_terminal, kmsgbuf.buf[i % sizeof(kmsgbuf.buf)]);
	}
#endif
}

void kputc(int ch, void *unused)
{
	/* put on kmsgbuf */
	kmsgbuf.buf[kmsgbuf.write++] = ch;
	if (kmsgbuf.write >= 4096)
		kmsgbuf.write = 0;
	if (kmsgbuf.read == kmsgbuf.write && ++kmsgbuf.read == kmsgbuf.write)
		kmsgbuf.read = 0;
	pac_putc(ch, NULL);
	if (system_terminal != nil) {
		fbterminal_putc(system_terminal, ch);
	}
}
