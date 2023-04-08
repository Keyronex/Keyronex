#!/usr/bin/env ksh93

USAGE+="[+NAME?amd64_run.sh -- run Keyronex under QEmu]"
USAGE+="[d:dax?Enable VirtIO-FS DAX.]"
USAGE+="[e:virtio-disk?Enable VirtIO-Disk.]"
USAGE+="[f:virtio-fs?Enable VirtIO-FS.]"
USAGE+="[i:iso?Path to ISO.]:?[/path/to/barebones.iso]"
USAGE+="[k:kvm?Enable hardware accelerated virtualisation.]"
USAGE+="[n:virtio-net?Enable VirtIO-NIC.]"
USAGE+="[q:qemu-exe?Path to QEmu executable.]:?[/path/to/qemu]"
USAGE+="[r:root?Path to FUSE/9p root directory.]:?[/path/to/root]"
USAGE+="[s:smp]#[smp:=4?Set number of cores.]"
USAGE+="[9:virtio-9p?Enable VirtIO-9P.]"

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

while getopts "$USAGE" optchar ; do
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
esac done

if [ "$iso" = "" ]; then
	echo "ISO file must be specified with -i /path/to/barebones.iso"
	exit 1
fi

if [ "$root" = "" ]; then
	echo "Root directory must be specified with -r /path/to/root"
	exit 1
fi

qemu_args=""
virtio_fs_args="-object memory-backend-file,id=mem,size=256M,mem-path=/dev/shm,share=on \
  -numa node,memdev=mem \
  -chardev socket,id=char0,path=/tmp/vhostqemu \
  -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=myfs"

(($dax)) && {
	echo "Using DAX" ;
	virtio_fs_args+=",cache-size=64M" ;
}

(($virtiodisk)) && qemu_args+="-drive file=hda.img,if=virtio "

(($virtionet)) && {
	qemu_args+="-net bridge,br=virbr0 -net nic,model=virtio "
}

(($virtio9p)) && {
	qemu_args+="-virtfs local,path=${root},security_model=none,\
mount_tag=root "
}

(($virtiofs)) && qemu_args+="${virtio_fs_args} "
(($kvm)) && qemu_args+="-enable-kvm "

qemu_args+="-serial stdio "
qemu_args+="-cdrom ${iso} "

echo "Launching: ${qemu_exe} ${qemu_args} -smp $smpnum -boot d"
echo ""
echo ""

${qemu_exe} ${qemu_args} --trace "v9fs_*" -m 256 -M q35 -smp $smpnum -boot d -s
