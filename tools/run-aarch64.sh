qemu_exe=qemu-system-aarch64
cores=1
memory=128

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
  -fsdev local,id=sysroot,security_model=none,path=build/aarch64/system-root"

qemu_args=

if [ "$kvm" = "1" ]; then
	qemu_args="${qemu_args} -enable-kvm"
	cpu="host"
	display="spice-app"
else
	cpu="cortex-a72"
	display="gtk"
fi

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

$qemu_exe -M virt -cpu ${cpu} -smp ${cores} \
  -device ramfb -device qemu-xhci -device usb-kbd \
  -m ${memory} \
  -bios OVMF.fd \
  -boot menu=on,splash-time=0 \
  -cdrom build/aarch64/barebones.iso \
  -boot d \
  -s \
  -display ${display} \
  ${qemu_args}
