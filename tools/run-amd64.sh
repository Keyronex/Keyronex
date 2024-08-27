PORT=amd64

qemu_args=

while getopts "9dknr:spq:" optchar; do
	case $optchar in
	9) virtio_9p=1 ;;
	d) virtio_disk=1 ;;
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	k) qemu_args="$qemu_args -enable-kvm" ;;
	#n) qemu_args="$qemu_args -drive file=test.img,if=none,id=nvm -device nvme,serial=deadbeef,drive=nvm" ;;
	p) pause=1 ;;
	#q) QEMU_EXE=$OPTARG;;
	*) usage ;;
esac done

QEMU_EXE="${QEMU_EXE:=qemu-system-x86_64}"
#QEMU_EXE=/ws/Compilers/qemu-8.0.4/bld/qemu-system-x86_64
qemu_args="$qemu_args -s"

virtio_9p_arg="-device pci-bridge,id=bridge0,chassis_nr=1 \
	-device virtio-9p-pci,id=pci9p,bus=bridge0,fsdev=sysroot,mount_tag=sysroot \
  -fsdev local,id=sysroot,security_model=none,path=build/amd64/system-root"

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

virtio_disk_arg="-device virtio-blk-pci,drive=drive0,id=virtblk0     -drive file=FAT16.img,if=none,id=drive0"
#virtio_scsi_arg="-device virtio-scsi-pci,id=virtscs0  -device scsi-hd,drive=drive0   -drive file=ext2.img,if=none,id=drive0"
#virtio_trace_arg=--trace "virtio_*"
# AHCI?
#

${QEMU_EXE} -smp 4 -hda test.img -m 24 -M q35 \
  -cdrom build/amd64/barebones.iso \
  ${virtio_gpu_arg} \
  ${virtio_trace_arg} \
  ${qemu_args}  -net tap,ifname=tap0,script=no,downscript=no -net nic,model=e1000e \
