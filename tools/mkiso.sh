SYSROOT=/tmp/keyronex-sysroot

DESTDIR=$SYSROOT ninja -C build install &&
tar -cf build/initrd.tar -C ${SYSROOT} ./ &&
mkdir -p build/isoroot &&
cp build/kernel/amd64/keyronex build/initrd.tar kernel/amd64/limine.cfg \
    limine/limine.sys limine/limine-cd.bin limine/limine-cd-efi.bin \
    build/isoroot &&
xorriso -as mkisofs -b limine-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-cd-efi.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  build/isoroot -o build/barebones.iso &&
build/limine-deploy build/barebones.iso
