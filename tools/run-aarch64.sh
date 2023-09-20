qemu_exe=qemu-system-aarch64

while getopts "r:spq:9" optchar; do
	case $optchar in
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	p) pause=1 ;;
        q) qemu_exe=$OPTARG ;;
	*) usage ;;
esac done

qemu_args=

if [ "$serial_stdio" = "1" ]; then
	qemu_args="${qemu_args} -serial stdio"
fi

if [ "$pause" = "1" ]; then
        qemu_args="${qemu_args} -S"
fi

virtio_disk_arg=
virtio_trace_arg=--trace "virtio_*"

$qemu_exe -M virt,gic-version=max \
  -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
  -boot menu=on,splash-time=0 \
  -cpu cortex-a72 \
  -smp 12 \
  -device ramfb \
  -cdrom build/aarch64/barebones.iso \
  -s \
  ${qemu_args}
