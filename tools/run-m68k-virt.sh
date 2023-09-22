while getopts "gr:sp9" optchar; do
	case $optchar in
	g) gpu=1 ;;
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	p) pause=1 ;;
	*) usage ;;
esac done

qemu_args=

if [ "$serial_stdio" = "1" ]; then
	qemu_args="${qemu_args} -serial stdio"
fi

if [ "$pause" = "1" ]; then
        qemu_args="${qemu_args} -S"
fi

if [ "$gpu" = "1" ]; then
        virtio_gpu_arg="-device virtio-gpu-device"
fi

virtio_disk_arg="-drive id=mydrive,file=ext2.img -device virtio-blk-device,drive=mydrive"
virtio_trace_arg=--trace "virtio_*"

#gdb -nx --args \
/ws/Projects/QemuBld/qemu-system-m68k -M virt \
  -kernel build/m68k/pkg-builds/kernel/loader/m68k-virt-loader/keyronex-loader-m68k-virt \
  -initrd build/m68k/pkg-builds/kernel/platform/m68k-virt/keyronex \
  ${virtio_disk_arg} \
  ${virtio_gpu_arg} \
  ${virtio_trace_arg} \
  -s  --trace "virtio_*" \
  ${qemu_args}
