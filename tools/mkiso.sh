if [ -z "${ARCH}" ]; then
	echo "Please set ARCH to the target architecture"
	exit 1
fi

if [ -z "${PLATFORM}" ]; then
	echo "Please set PLATFORM to the target platform"
	exit 1
fi

BUILD_DIR=build/${ARCH}
LIMINE_EXE=${BUILD_DIR}/pkg-builds/kernel/limine
KERNEL_EXE=${BUILD_DIR}/pkg-builds/kernel/platform/${PLATFORM}/keyronex
CONFIG=tools/limine.conf

LIMINE=vendor/limine
ISOROOT=build/${ARCH}/isoroot
ISO=build/${ARCH}/barebones.iso

mkdir -p ${ISOROOT}/EFI/BOOT
cp -v ${LIMINE}/limine-bios.sys ${LIMINE}/limine-bios-cd.bin \
    ${LIMINE}/limine-uefi-cd.bin ${ISOROOT}/
cp -v ${LIMINE}/BOOTX64.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTIA32.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTAA64.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTRISCV64.EFI ${ISOROOT}/EFI/BOOT/

cp ${KERNEL_EXE} ${CONFIG} ${ISOROOT}/

cp ${CONFIG} ${ISOROOT}/limine.conf

xorriso -as mkisofs -b limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    ${ISOROOT} -o ${ISO}

${LIMINE_EXE} bios-install ${ISO}
