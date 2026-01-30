if [ -z "${ARCH}" ]; then
    ARCH="amd64"
fi
BUILD_DIR=build/${ARCH}

LIMINE_EXE=${BUILD_DIR}/pkg-builds/kernel/limine
KERNEL_EXE=${BUILD_DIR}/pkg-builds/kernel/common/keyronex
MODULE_DIR=${BUILD_DIR}/pkg-builds/kernel/kernext
CONFIG=tools/limine.conf
LIMINE=vendor/limine

IMAGE_NAME=${BUILD_DIR}/barebones.iso
ISOROOT=${BUILD_DIR}/isoroot

rm -f ${IMAGE_NAME}
rm -rf ${ISOROOT}
mkdir -p ${ISOROOT}

mkdir -p ${ISOROOT}/EFI/BOOT
cp -v ${LIMINE}/limine-bios.sys ${LIMINE}/limine-bios-cd.bin ${LIMINE}/limine-uefi-cd.bin ${ISOROOT}/
cp -v ${LIMINE}/BOOTX64.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTIA32.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTAA64.EFI ${ISOROOT}/EFI/BOOT/
cp -v ${LIMINE}/BOOTRISCV64.EFI ${ISOROOT}/EFI/BOOT/

cp ${KERNEL_EXE} ${CONFIG} ${ISOROOT}/

#mkdir -p ${ISOROOT}/usr/lib/kernext
#for dir in ${MODULE_DIR}/*; do
#	if [ -d "$dir" ]; then
#		dir_name=$(basename "$dir")
#		cp -v "${dir}/${dir_name}.kernext" "${ISOROOT}/usr/lib/kernext/"
#		echo "    module_path: boot():/usr/lib/kernext/${dir_name}.kernext" \
#		    >> ${ISOROOT}/limine.conf
#	fi
#done

xorriso -as mkisofs -b limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    ${ISOROOT} -o ${IMAGE_NAME}
${LIMINE_EXE} bios-install ${IMAGE_NAME}
