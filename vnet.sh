qemu-system-x86_64 -cdrom build/barebones.iso  -s  -serial stdio \
  -drive file=hda.img,if=virtio \
  -netdev tap,id=mynet0,ifname=tap0,script=no,downscript=no \
  -device virtio-net-pci,netdev=mynet0 \
  -smp 4 -boot d -enable-kvm
