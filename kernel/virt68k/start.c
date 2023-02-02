typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

/*!
 * adapted from:
 * https://android.googlesource.com/platform/external/qemu/+/master/docs/GOLDFISH-VIRTUAL-HARDWARE.TXT
 */

/*! goldfish tty registers */
enum gftty_reg {
    GFTTY_PUT_CHAR      = 0x00,
    GFTTY_BYTES_READY   = 0x04,
    GFTTY_CMD           = 0x08,
    GFTTY_DATA_PTR      = 0x10,
    GFTTY_DATA_LEN      = 0x14,
};

/*! goldfish tty commands*/
enum gftty_cmd {
    GFTTY_CMD_INT_DISABLE   = 0x00,
    GFTTY_CMD_INT_ENABLE    = 0x01,
    GFTTY_CMD_WRITE_BUFFER  = 0x02,
    GFTTY_CMD_READ_BUFFER   = 0x03,
};

/*! hardcoded value i found in qemu, lmao */
volatile char * gftty_regs = (void *)0xff008000;

static uint32_t gftty_read(unsigned int reg) {
    return *((uint32_t*)&gftty_regs[reg]);
}

static void gftty_write(enum gftty_reg reg, uint32_t val) {
    *((uint32_t*)&gftty_regs[reg]) = val;
}

void gftty_init(void) {
    gftty_write(GFTTY_CMD, GFTTY_CMD_INT_DISABLE);
}

void gftty_putc(int c) {
    gftty_write(GFTTY_PUT_CHAR, c);
}

void cstart(void) {
	gftty_init();
	gftty_putc('h');
	for (;;) ;
}
