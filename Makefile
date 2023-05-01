KRX_BUILDDIR=build
KRX_SYSROOT=build/system-root
KERNEL_EXE=${KRX_SYSROOT}/boot/keyronex
LIMINE=vendor/limine-binary
ISO_DIR=build/isoroot
ISO=build/barebones.iso

all: build/

	(cd build && xbstrap install -c --all)

# check if mlibc/meson.build exists, if not, error
mlibc/meson.build:
	    @if [ ! -f $@ ]; then \
			echo " -- mlibc/meson.build is missing"; \
			echo " -- Run git submodule update --init --recursive"; \
			exit 1; \
        fi

# create the build/ directory and set it up
build/: mlibc/meson.build
	mkdir -p build/
	(cd build/ && xbstrap init ..)
	touch build/

rebuild-kernel: build/
	(cd build && xbstrap install --rebuild mlibc-headers mlibc keyronex-kernel)

iso:
	rm -rf ${ISO_DIR}
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

	@build/pkg-builds/keyronex-kernel/limine-deploy ${ISO}

	@echo "==> Build is done."
	@echo "    Run Keyronex like this:"
	@echo "    tools/amd64_run.sh -9 -k -r ${KRX_SYSROOT} -i ${ISO}"
	@echo "    (or use the 'run' Makefile target)"

run:
	@tools/amd64_run.sh -9 -k -r ${KRX_SYSROOT} -i ${ISO} -q /tmp/qemu-8.0.0/build/qemu-system-x86_64

runnosmp:
	@tools/amd64_run.sh -9 -s 1 -r ${KRX_SYSROOT} -i ${ISO}
