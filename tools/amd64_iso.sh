LIMINE="vendor/limine-binary"
mkdir -p build/isoroot &&
cp build/system-root/boot/keyronex \
    ${LIMINE}/limine.sys ${LIMINE}/limine-cd.bin ${LIMINE}/limine-cd-efi.bin \
    build/isoroot &&
cp tools/amd64_limine.cfg build/isoroot/limine.cfg &&
xorriso -as mkisofs -b limine-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-cd-efi.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  build/isoroot -o build/barebones.iso &&
build/pkg-builds/keyronex-kernel/limine-deploy build/barebones.iso
