#!/bin/sh

usage() {
	echo "Usage: $0 [-a <arch>] [-c <cores>] [-d] [-e] [-g] [-h] [-k] [-p] [-q <qemu_exe>] [-9]"
	echo "  -9          : Enable virtio 9p"
	echo "  -a <arch>   : Architecture to run (default: amd64)"
	echo "  -c <cores>  : Number of CPU cores to use (default: 1)"
	echo "  -d          : Enable virtio disk"
	echo "  -e          : Enable e1000 network device"
	echo "  -g          : Enable virtio GPU"
	echo "  -h          : Display this help message"
	echo "  -k          : Enable KVM"
	echo "  -p          : Pause after start"
	echo "  -q <qemu>   : Specify QEMU executable"
}

while getopts "9a:c:deghkpq:" opt; do
	case "$opt" in
	9) virtio_9p=1 ;;
	a) ARCH="$OPTARG" ;;
	c) cores=$OPTARG ;;
	d) virtio_disk=1 ;;
	e) e1000=1 ;;
	g) virtio_gpu=1 ;;
	h) usage ; exit 0 ;;
	k) kvm=1 ;;
	p) pause=1 ;;
	q) QEMU_EXE="$OPTARG" ;;
	?) usage ; exit 0 ;;
	esac
done

if [ -z "${ARCH}" ]; then
	ARCH="amd64"
fi

if [ -z "${QEMU_EXE}" ]; then
	if [ "${ARCH}" = "amd64" ]; then
		QEMU_EXE=qemu-system-x86_64
	else
		QEMU_EXE=qemu-system-${ARCH}
	fi
fi

if [ -z "${cores}" ]; then
	cores=1
fi

v9p_root="build/${ARCH}/system-root"

if [ "${ARCH}" = "m68k" ]; then
	virtio_9p_arg="-device virtio-9p-device,fsdev=sysroot,mount_tag=sysroot"
elif [ "${ARCH}" = "amd64" ]; then
	virtio_9p_arg="-device pci-bridge,id=bridge0,chassis_nr=1 \
	    -device virtio-9p-pci,id=pci9p,bus=bridge0,fsdev=sysroot,mount_tag=sysroot"
else
	virtio_9p_arg="-device virtio-9p-pci,fsdev=sysroot,mount_tag=sysroot"
fi

virtio_9p_arg="${virtio_9p_arg} -fsdev local,id=sysroot,security_model=none,path=${v9p_root}"

qemu_args="-serial stdio -s"

if [ "$pause" = "1" ]; then
	qemu_args="${qemu_args} -S"
fi

if [ "$virtio_gpu" = "1" ]; then
	qemu_args="${qemu_args} -device virtio-gpu-device"
fi

if [ "$virtio_disk" = "1" ]; then
	qemu_args="${qemu_args} ${virtio_disk_arg}"
fi

if [ "$virtio_9p" = "1" ]; then
	qemu_args="${qemu_args} ${virtio_9p_arg}"
fi

if [ "$e1000" = "1" ]; then
	qemu_args="${qemu-args} -net tap,ifname=tap0,script=no,downscript=no -net nic,model=e1000e"
else
	qemu_args="${qemu_args} -net none"
fi

ISO="build/${ARCH}/barebones.iso"

echo "QEMU ARGS: ${qemu_args}"

case "$ARCH" in
	aarch64) ${QEMU_EXE} -M virt -smp ${cores} -device ramfb \
	    -device qemu-xhci -device usb-kbd -m 128 -cpu cortex-a72 \
	    -bios ovmf-aarch64/OVMF.fd -boot menu=on,splash-time=0 \
	    -cdrom ${ISO} -boot d ${qemu_args}
		;;

	amd64) ${QEMU_EXE} -cdrom ${ISO} -no-reboot \
	    ${qemu_args} -smp ${cores} ;;

	m68k) ${QEMU_EXE} -M virt -m 128 \
	    -kernel build/m68k/pkg-builds/m68k-virt-loader/lisp \
	    ${qemu_args} ;;

	riscv64) ${QEMU_EXE} -M virt,aia=aplic -cpu rv64 -device ramfb \
	    -device qemu-xhci -device usb-kbd  \
	    -drive if=pflash,unit=0,format=raw,file=ovmf-riscv64/OVMF.fd \
	    -boot menu=on,splash-time=1 \
	    -device virtio-scsi-pci,id=scsi -device scsi-cd,drive=cd0 \
	    -drive id=cd0,format=raw,file=${ISO},if=none ${qemu_args}
		;;

	*) echo "Unsupported architecture: $ARCH" ; usage; exit 1 ;;
esac
