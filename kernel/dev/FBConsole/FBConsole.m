#include <sys/sysmacros.h>

#include <dev/FBConsole/FBConsole.h>
#include <dev/dev.h>
#include <kern/kmem.h>

#include <vfs/vfs.h>

extern char sun12x22[], nbsdbold[];

FBConsole *syscon = nil;
static int termnum = 0;
tty_t	  *sctty;

static int fbtopen(dev_t dev, vnode_t **out, int mode);
static int fbtputch(void *data, int ch);

@implementation FBConsole

+ (BOOL)probeWithFB:(LimineFB *)fb
{
	if (syscon == nil) {
		syscon = [[self alloc] initWithFB:fb];
		return YES;
	}
	return NO;
}

- (id)initWithFB:(LimineFB *)fb
{
	self = [super initWithProvider:fb];

	_fb = fb;
	style = (struct style_t) { DEFAULT_ANSI_COLOURS,
		DEFAULT_ANSI_BRIGHT_COLOURS, 0xFFFFFFFF, 0x00000000, 40, 0 };
	back = (struct background_t) { NULL, 0, 0x00000000 };
	frm = (struct framebuffer_t) { .address = (uintptr_t)fb.base,
		.width = fb.width,
		.height = fb.height,
		.pitch = fb.pitch };
	font = (struct font_t) {
		.address = (uintptr_t)nbsdbold,
		.spacing = 1,
		.scale_x = 1,
		.scale_y = 1,
	};

	tty.termios.c_cc[VINTR] = 0x03;
	tty.termios.c_cc[VEOL] = '\n';
	tty.termios.c_cc[VEOF] = '\0';
	tty.termios.c_cflag = TTYDEF_CFLAG;
	tty.termios.c_iflag = TTYDEF_IFLAG;
	tty.termios.c_lflag = TTYDEF_LFLAG;
	tty.termios.c_oflag = TTYDEF_OFLAG;
	tty.termios.ibaud = tty.termios.obaud = TTYDEF_SPEED;

	// waitq_init(&tty.wq_canon);
	// waitq_init(&tty.wq_noncanon);

	tty.putch = fbtputch;
	tty.data = self;

	kmem_asprintf(&m_name, "FBConsole%d", termnum++);

	term_init(&term, NULL, false);
	term_vbe(&term, frm, font, style, back);

	if (syscon == nil) {
		cdevsw_t cdev;
		int	 maj;

		cdev.is_tty = true;
		cdev.private = self;
		cdev.open = fbtopen;
		cdev.read = tty_read;
		cdev.write = tty_write;
		// cdev.kqfilter = tty_kqfilter;

		syscon = self;

		maj = cdevsw_attach(&cdev);
		devfs_make_node(makedev(maj, 0), "console");
		sctty = &tty;
		(void)maj;

		for (int i = msgbuf.read; i != msgbuf.write; i++) {
			if (i >= sizeof(msgbuf.buf))
				i = 0;
			term_putchar(&term, msgbuf.buf[i]);
		}
		term_double_buffer_flush(&term);
	}

	[self registerDevice];
	DKLogAttach(self);

	return self;
}

#if 0
- (tty_t *)tty
{
	return &tty;
}

- (void)input:(int)c
{
	tty_input(&tty, c);
}

- (void)inputChars:(const char *)cs
{
	while (*cs != '\0')
		tty_input(&tty, *cs++);
}
#endif

- (void)write:(void *)buf len:(size_t)len
{
	term_write(&term, (uint64_t)buf, len);
}

- (void)putc:(int)c
{
	term_putchar(&term, c);
	if (c == '\n')
		term_double_buffer_flush(&term);
}

- (void)flush
{
	term_double_buffer_flush(&term);
}

@end

static int
fbtopen(dev_t dev, vnode_t **out, int mode)
{
	return 0;
}

static int
fbtputch(void *data, int c)
{
	[(FBConsole *)data putc:c];
	[(FBConsole *)data flush];
	return 0;
}

void
sysconputc(int c)
{
	if (syscon) {
		[syscon putc:c];
	}
}

void
sysconflush()
{
	[syscon flush];
}
