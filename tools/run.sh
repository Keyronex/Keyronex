#!/bin/sh

usage() {
	echo "Usage: $0 [-9] [-a <arch>] [-c <cores>] [-h] [-k] [-n] [-p] [-q <qemu_exe>] [-u]"
	echo "  -9          : Enable virtio 9p"
	echo "  -a <arch>   : Architecture to run (default: amd64)"
	echo "  -c <cores>  : Number of CPU cores to use (default: 1)"
	echo "  -h          : Display this help message"
	echo "  -k          : Enable KVM"
	echo "  -n          : Enable virtio-nic"
	echo "  -p          : Pause on start (qemu -S)"
	echo "  -q <qemu>   : Specify QEMU executable"
}

while getopts "9a:c:deghkpq:u" opt; do
	case "$opt" in
	9) virtio_9p=1 ;;
	a) ARCH="$OPTARG" ;;
	c) cores=$OPTARG ;;
	h) usage ; exit 0 ;;
	k) kvm=1 ;;
	n) virtio_nic=1 ;;
	p) pause=1 ;;
	q) QEMU_EXE="$OPTARG" ;;
	?) usage ; exit 0 ;;
	esac
done

if [ -z "${ARCH}" ]; then
    ARCH="amd64"
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
if [ "$virtio_9p" = "1" ]; then
	qemu_args="${qemu_args} ${virtio_9p_arg}"
fi

if [ -z "${cores}" ]; then
	cores=4
fi

if [ "$kvm" = "1" ]; then
	qemu_args="${qemu_args} -enable-kvm -cpu host"
fi

virtio_net=1
virtio_net_arg="-netdev tap,id=net0,ifname=tap0,script=no,downscript=no"
if [ "${ARCH}" = "m68k" ]; then
	virtio_net_arg="${virtio_net_arg} -device virtio-net-device,netdev=net0"
else
	virtio_net_arg="${virtio_net_arg} -net nic,model=virtio,netdev=net0"
fi
virtio_net_arg="${virtio_net_arg} -object filter-dump,id=f1,netdev=net0,file=/tmp/net0.pcap"
if [ "${virtio_net}" = "1" ]; then
	qemu_args="${qemu_args} ${virtio_net_arg}"

fi

iso="build/${ARCH}/barebones.iso"

qemu_args="${qemu_args} -s"

# qemu_args="${qemu_args} -monitor telnet:127.0.0.1:5555,server,nowait"
# qemu_args="${qemu_args} -S"

case "$ARCH" in
	aarch64) qemu-system-aarch64 -M virt -smp ${cores} -device ramfb \
	    -device qemu-xhci -device usb-kbd -m 128 -cpu cortex-a72 \
	    -bios ovmf-aarch64/OVMF.fd -boot menu=on,splash-time=0 \
	    -cdrom ${iso} -boot d ${qemu_args} -serial stdio
	    ;;

	amd64) qemu-system-x86_64 -cdrom ${iso} -no-reboot \
	    ${qemu_args} -smp ${cores} -serial stdio -M q35 -cpu host -enable-kvm \
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
