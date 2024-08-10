LIMINE=vendor/limine
ISOROOT=build/aarch64/isoroot
ISO=build/aarch64/barebones.iso

mkdir -p ${ISOROOT} &&

cp build/aarch64/pkg-builds/kernel/kernel/platform/aarch64-virt/keyronex \
    ${LIMINE}/limine-bios.sys ${LIMINE}/limine-bios-cd.bin ${LIMINE}/limine-uefi-cd.bin \
    ${ISOROOT} &&

cp tools/limine-aarch64.cfg ${ISOROOT}/limine.conf &&

xorriso -as mkisofs \
  -no-emul-boot -boot-load-size 4 -boot-info-table \
  --efi-boot limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
  --protective-msdos-label \
  ${ISOROOT} -o ${ISO}
