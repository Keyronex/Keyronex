qemu_exe=/opt/qemu-git/bin/qemu-system-riscv64
cores=1
memory=128
display="gtk"

while getopts "S:c:km:r:spq:9" optchar; do
	case $optchar in
	9) virtio_9p=1 ;;
	c) cores=$OPTARG ;;
	k) kvm=1 ;;
	m) memory=$OPTARG ;;
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	p) pause=1 ;;
	q) qemu_exe=$OPTARG ;;
	*) usage ;;
esac done

virtio_9p_arg="-device virtio-9p-pci,fsdev=sysroot,mount_tag=sysroot \
  -fsdev local,id=sysroot,security_model=none,path=build/riscv64/system-root"

qemu_args=

if [ "$kvm" = "1" ]; then
	qemu_args="${qemu_args} -enable-kvm"
fi

if [ "$serial_stdio" = "1" ]; then
	qemu_args="${qemu_args} -serial stdio"
fi

if [ "$pause" = "1" ]; then
        qemu_args="${qemu_args} -S"
fi

if [ "$virtio_9p" = "1" ]; then
	qemu_args="${qemu_args} -${virtio_9p_arg}"
fi

virtio_disk_arg=
virtio_trace_arg=--trace "virtio_*"

${qemu_exe} -M virt -m ${memory} -cpu rv64 -device ramfb \
	-device qemu-xhci -device usb-kbd  \
	-drive if=pflash,unit=0,format=raw,file=ovmf-riscv64/OVMF.fd \
	-boot menu=on,splash-time=1 \
	-device virtio-scsi-pci,id=scsi -device scsi-cd,drive=cd0 \
	-drive id=cd0,format=raw,file=build/riscv64/barebones.iso,if=none \
	-serial stdio -s \
	${qemu_args}
