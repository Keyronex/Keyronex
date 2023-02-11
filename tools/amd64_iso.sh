LIMINE="external/limine-binary"
mkdir -p build/isoroot &&
cp build/mlxkern/hl/amd64/mlxkern \
    ${LIMINE}/limine.sys ${LIMINE}/limine-cd.bin ${LIMINE}/limine-cd-efi.bin \
    build/isoroot &&
cp tools/amd64_limine.cfg build/isoroot/limine.cfg &&
xorriso -as mkisofs -b limine-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-cd-efi.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  build/isoroot -o build/barebones.iso &&
build/limine-deploy build/barebones.iso
