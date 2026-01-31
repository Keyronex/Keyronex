#!/bin/sh

if [ -z "${ARCH}" ]; then
    ARCH="amd64"
fi

iso="build/${ARCH}/barebones.iso"

cores=4
qemu_args="-s"

case "$ARCH" in
	aarch64) qemu-system-aarch64 -M virt -smp ${cores} -device ramfb \
	    -device qemu-xhci -device usb-kbd -m 128 -cpu cortex-a72 \
	    -bios ovmf-aarch64/OVMF.fd -boot menu=on,splash-time=0 \
	    -cdrom ${iso} -boot d ${qemu_args} -serial stdio
	    ;;

	amd64) qemu-system-x86_64 -cdrom ${iso} -no-reboot \
	    ${qemu_args} -smp ${cores} -serial stdio
	    ;;

	m68k) qemu-system-m68k -M virt -m 128 \
	    -kernel build/m68k/pkg-builds/m68k-virt-loader/lisp \
	    -serial stdio \
	    ${qemu_args}
	    ;;

	riscv64) qemu-system-riscv64 -M virt,aia=aplic -cpu rv64 -device ramfb \
	    -device qemu-xhci -device usb-kbd  \
	    -drive if=pflash,unit=0,format=raw,file=ovmf-riscv64/OVMF.fd \
	    -boot menu=on,splash-time=1 \
	    -device virtio-scsi-pci,id=scsi -device scsi-cd,drive=cd0 \
	    -drive id=cd0,format=raw,file=${iso},if=none ${qemu_args} -serial stdio
	    ;;

	*) echo "Unsupported architecture: $ARCH" ; usage; exit 1 ;;
esac
