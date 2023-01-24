qemu-system-x86_64 -cdrom build/barebones.iso  -s  -serial stdio \
  -drive file=hda.img,if=virtio \
  -netdev bridge,br=vmbridge,id=bridge0 \
  -device virtio-net-pci,netdev=bridge0 \
  -smp 1 -enable-kvm
