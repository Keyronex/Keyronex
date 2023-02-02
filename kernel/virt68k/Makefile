all:
	m68k-scalux-gcc -mcpu=68040 start.S -ggdb -c -o start.S.o
	m68k-scalux-gcc -mcpu=68040 -ffreestanding -nostdlib start.c -ggdb -c -o start.c.o
	m68k-scalux-ld -nostdlib start.c.o start.S.o -Ttext=0x1000 -o kernel.elf

runqemu:
	qemu-system-m68k -M virt -kernel kernel.elf -S -s
