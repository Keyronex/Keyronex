if [ $# -eq 0 ]
  then
    debug="-enable-kvm"
  else
    debug="$@"
fi

qemu-system-x86_64 -cdrom build/barebones.iso -s -serial stdio ${debug} \
	-drive file=hda.img,if=virtio \
	-smp 4 \
	-boot d
