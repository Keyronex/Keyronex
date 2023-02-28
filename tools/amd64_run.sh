#!/usr/bin/env ksh93


USAGE+="[+NAME?amd64_run.sh -- run Keyronex under QEmu]"
USAGE+="[d:dax?Enable VirtIO-FS DAX.]"
USAGE+="[f:virtiofs?Enable VirtIO-FS.]"
USAGE+="[k:kvm?Enable hardware accelerated virtualisation.]"
USAGE+="[q:qemu-exe?Path to QEmu executable.]:?[/path/to/qemu]"
USAGE+="[s:smp]#[smp:=4?Set number of cores.]"

qemu_exe=qemu-system-x86_64
dax=0
virtiofs=0
kvm=0
smpnum=4

while getopts "$USAGE" optchar ; do
	case $optchar in
	d) dax=1 ;;
	f) virtiofs=1 ;;
	k) kvm=1 ;;
	q) qemu_exe=$OPTARG ;;
	s) smpnum=$OPTARG ;;
esac done

qemu_args=""
virtio_fs_args="-object memory-backend-file,id=mem,size=128M,mem-path=/dev/shm,share=on \
  -numa node,memdev=mem \
  -chardev socket,id=char0,path=/tmp/vhostqemu \
  -device vhost-user-fs-pci,queue-size=1024,chardev=char0,tag=myfs"

(($dax)) && {
	echo "Using DAX" ;
	virtio_fs_args+=",cache-size=64M" ;
}

(($virtiofs)) && qemu_args+="${virtio_fs_args} "
(($kvm)) && qemu_args+="-enable-kvm "

qemu_args+="-drive file=hda.img,if=virtio "
qemu_args+="-serial stdio  "
qemu_args+="-cdrom build/barebones.iso  "

echo "Launching: ${qemu_exe} ${qemu_args} -smp $smpnum -boot d"
echo ""
echo ""

${qemu_exe} ${qemu_args} -smp $smpnum -boot d -s
