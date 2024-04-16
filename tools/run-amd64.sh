PORT=amd64

qemu_args=

while getopts "knr:spq:9" optchar; do
	case $optchar in
	r) root=$OPTARG ;;
	s) serial_stdio=1 ;;
	k) qemu_args="$qemu_args -enable-kvm" ;;
	#n) qemu_args="$qemu_args -drive file=test.img,if=none,id=nvm -device nvme,serial=deadbeef,drive=nvm" ;;
	p) pause=1 ;;
	#q) QEMU_EXE=$OPTARG;;
	*) usage ;;
esac done

QEMU_EXE="${QEMU_EXE:=qemu-system-x86_64}"
qemu_args="$qemu_args -s"
#QEMU_EXE=/opt/qemusept/bin/qemu-system-x86_64
if [ "$serial_stdio" = "1" ]; then
	qemu_args="${qemu_args} -serial stdio"
fi

if [ "$pause" = "1" ]; then
	qemu_args="${qemu_args} -S"
fi

if [ "$gpu" = "1" ]; then
	virtio_gpu_arg="-device virtio-gpu-device"
fi

#virtio_disk_arg="-drive id=mydrive,file=ext2.img,if=virtio"
#virtio_disk_arg="-device virtio-blk-pci,drive=drive0,id=virtblk0     -drive file=ext2.img,if=none,id=drive0"
virtio_disk_arg="-device virtio-scsi-pci,id=virtscs0  -device scsi-hd,drive=drive0   -drive file=ext2.img,if=none,id=drive0"
#virtio_trace_arg=--trace "virtio_*"

# AHCI?
#
AHCI_DISK=

${QEMU_EXE} -smp 4 -hda test.img -M q35 \
  -cdrom build/amd64/barebones.iso \
  ${virtio_disk_arg} \
  ${virtio_gpu_arg} \
  ${virtio_trace_arg} \
  ${qemu_args}
