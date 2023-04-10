SUBDIR += basefiles mlibc kernel coreutils bash ncurses nano

#KERNEL_EXE=${KP_STAGEDIR}/kernel/boot/keyronex
KERNEL_EXE=${KP_SYSROOT}/boot/keyronex
LIMINE=vendor/limine-binary
ISO_DIR=${KP_WORKDIR}/isoroot
ISO=${KP_WORKDIR}/barebones.iso

rebuild-kernel:
	rm -f ${KP_WORKDIR}/mlibc/.build-done
	rm -f ${KP_WORKDIR}/kernel/.build-done
	${MAKE} ${MAKEFLAGS}

iso:
	@mkdir -p ${ISO_DIR}

	@cp ${KERNEL_EXE} \
		${LIMINE}/limine.sys ${LIMINE}/limine-cd.bin \
		${LIMINE}/limine-cd-efi.bin \
		${ISO_DIR}

	@cp tools/amd64_limine.cfg ${ISO_DIR}/limine.cfg

	@xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-cd-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		${ISO_DIR} -o ${ISO}

	@${KP_WORKDIR}/kernel/limine-deploy ${ISO}

	@echo "==> Build is done."
	@echo "    Run Keyronex like this:"
	@echo "    tools/amd64_run.sh -9 -k -r ${KP_SYSROOT} -i ${ISO}"
	@echo "    (or use the 'run' Makefile target)"

run:
	@tools/amd64_run.sh -9 -k -r ${KP_SYSROOT} -i ${ISO}


.include "buildsup/krx.kp.mk"
.include <bsd.subdir.mk>