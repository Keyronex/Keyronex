if [ $# -eq 0 ]
  then
    debug="-enable-kvm"
  else
    debug="-d int -no-reboot"
fi

qemu-system-x86_64 -cdrom build/barebones.iso -serial stdio ${debug}
