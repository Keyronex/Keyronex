qemu-system-riscv64 -M virt -m 128 -cpu rv64 -device ramfb \
                -device qemu-xhci -device usb-kbd  \
                -drive if=pflash,unit=0,format=raw,file=ovmf-riscv64/OVMF.fd \
                -boot menu=on,splash-time=1 \
                -device virtio-scsi-pci,id=scsi -device scsi-cd,drive=cd0 \
                -drive id=cd0,format=raw,file=build/riscv64/barebones.iso \
                -serial stdio -s
