while getopts "9dgr:sp" optchar; do
	case $optchar in
	9) virtio_9p=1 ;;
	d) virtio_disk=1 ;;
	g) gpu=1 ;;
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	p) pause=1 ;;
	*) usage ;;
esac done

qemu_args=

virtio_9p_arg="-device virtio-9p-device,fsdev=sysroot,mount_tag=sysroot \
  -fsdev local,id=sysroot,security_model=none,path=build/m68k/system-root"
virtio_disk_arg="-drive id=mydrive,file=FAT16.img -device virtio-blk-device,drive=mydrive"
#virtio_trace_arg="--trace "virtio_*""

if [ "$serial_stdio" = "1" ]; then
	qemu_args="${qemu_args} -serial stdio"
fi

if [ "$pause" = "1" ]; then
	qemu_args="${qemu_args} -S"
fi

if [ "$gpu" = "1" ]; then
	virtio_gpu_arg="-device virtio-gpu-device"
fi

if [ "$virtio_disk" = "1" ]; then
	qemu_args="${qemu_args} -${virtio_disk_arg}"
fi

if [ "$virtio_9p" = "1" ]; then
	qemu_args="${qemu_args} -${virtio_9p_arg}"
fi

qemu-system-m68k -M virt -m 9 \
  -kernel build/m68k/pkg-builds/m68k-virt-loader/lisp \
  ${virtio_gpu_arg} \
  ${virtio_trace_arg} \
  -s \
  ${qemu_args}
