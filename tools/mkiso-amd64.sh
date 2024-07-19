LIMINE=vendor/limine
ISOROOT=build/amd64/isoroot
ISO=build/amd64/barebones.iso

mkdir -p ${ISOROOT} &&

cp build/amd64/pkg-builds/kernel/kernel/platform/amd64/keyronex \
    ${LIMINE}/limine-bios.sys ${LIMINE}/limine-bios-cd.bin ${LIMINE}/limine-uefi-cd.bin \
    ${ISOROOT} &&

cp tools/limine-amd64.cfg ${ISOROOT}/limine.cfg &&

xorriso -as mkisofs -b limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
  --protective-msdos-label \
  ${ISOROOT} -o ${ISO}

build/amd64/pkg-builds/kernel/limine bios-install ${ISO}
