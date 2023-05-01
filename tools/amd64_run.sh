#!/bin/sh

usage() {
	echo "Usage: $0 [-d] [-e] [-f] [-i path] [-k] [-n] [-q path] [-r path] [-s num] [-9]" 1>&2
	echo "  -d			Enable VirtIO-FS DAX." 1>&2
	echo "  -e			Enable VirtIO-Disk." 1>&2
	echo "  -f			Enable VirtIO-FS." 1>&2
	echo "  -i path		Path to ISO file." 1>&2
	echo "  -k			Enable hardware accelerated virtualisation." 1>&2
	echo "  -n			Enable VirtIO-NIC." 1>&2
	echo "  -q path		Path to QEMU executable." 1>&2
	echo "  -r path		Path to FUSE/9p root directory." 1>&2
	echo "  -s num		Set number of cores (default is 4)." 1>&2
	echo "  -9			Enable VirtIO-9P." 1>&2
	exit 1
}

qemu_exe=qemu-system-x86_64
dax=0
virtiodisk=0
virtiofs=0
virtionet=0
virtio9p=0
iso=
root=
kvm=0
smpnum=4

while getopts "defi:knq:r:s:9" optchar; do
	case $optchar in
	d) dax=1 ;;
	f) virtiofs=1 ;;
	i) iso=$OPTARG ;;
	n) virtionet=1 ;;
	k) kvm=1 ;;
	q) qemu_exe=$OPTARG ;;
	r) root=$OPTARG ;;
	s) smpnum=$OPTARG ;;
	9) virtio9p=1 ;;
	*) usage ;;
esac done

if [ "$iso" = "" ]; then
	echo "ISO file path must be specified with -i" 1>&2
	usage
fi

if [ "$root" = "" ]; then
	echo "Root directory must be specified with -r" 1>&2
	usage
fi

qemu_args=""
virtio_fs_args="-object memory-backend-file,id=mem,size=256M,mem-path=/dev/shm,share=on \
  -numa node,memdev=mem \
  -chardev socket,id=char0,path=/tmp/vhostqemu \
  -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=myfs"

if [ "$dax" = "1" ]; then
	echo "Using DAX"
	virtio_fs_args="${virtio_fs_args},cache-size=64M"
fi

if [ "$virtiodisk" = "1" ]; then
	qemu_args="${qemu_args} -drive file=hda.img,if=virtio"
fi

if [ "$virtionet" = "1" ]; then
	qemu_args="${qemu_args} -net bridge,br=virbr0 -net nic,model=virtio"
fi

if [ "$virtio9p" = "1" ]; then
	qemu_args="${qemu_args} -virtfs local,path=${root},security_model=none,mount_tag=root"
fi

if [ "$virtiofs" = "1" ]; then
	qemu_args="${qemu_args} ${virtio_fs_args}"
fi

if [ "$kvm" = "1" ]; then
	qemu_args="${qemu_args} -enable-kvm"
fi

qemu_args="${qemu_args} -serial stdio -cdrom ${iso}"

echo "Launching: ${qemu_exe} ${qemu_args} -smp $smpnum -boot d"
echo ""
echo ""

${qemu_exe} ${qemu_args} --trace "v9fs_*" -m 256 -M q35 -smp $smpnum -boot d -s
