ARCH?=amd64
BUILD_DIR=build/$(ARCH)
SYSROOT_DIR=${BUILD_DIR}/system-root
BUILD_SETUP_TARGET=${BUILD_DIR}/.setup

ifeq ($(ARCH), amd64)
	TARGET_TRIPLE := x86_64-keyronex
	PLATFORM ?= amd64
else ifeq ($(ARCH), m68k)
	TARGET_TRIPLE := m68k-keyronex
	PLATFORM ?= m68k-virt
	PLATFORM_EXTRAS=m68k-virt-loader
else ifeq ($(ARCH), aarch64)
	TARGET_TRIPLE := aarch64-keyronex
	PLATFORM ?= aarch64-virt
else ifeq ($(ARCH), riscv64)
	TARGET_TRIPLE := riscv64-keyronex
	PLATFORM ?= riscv64-virt
else
	$(error Unsupported ARCH: $(ARCH))
endif

all: build-all

# check if mlibc/meson.build exists, if not, error
mlibc/meson.build:
	@if [ ! -f $@ ]; then \
		echo " -- mlibc/meson.build is missing"; \
		echo " -- Run git submodule update --init --recursive"; \
	fi

# create the build/ directory and set it up
${BUILD_SETUP_TARGET}:
	mkdir -p ${BUILD_DIR}
	echo "define_options:" > ${BUILD_DIR}/bootstrap-site.yml
	echo "    arch: ${ARCH}" >> ${BUILD_DIR}/bootstrap-site.yml
	echo "    arch-triple: ${TARGET_TRIPLE}" >> ${BUILD_DIR}/bootstrap-site.yml
	echo "labels:" >> ${BUILD_DIR}/bootstrap-site.yml
	echo "    ban:" >> ${BUILD_DIR}/bootstrap-site.yml
	echo "      - no_${ARCH}" >> ${BUILD_DIR}/bootstrap-site.yml
	(cd ${BUILD_DIR} && xbstrap init ../..)
	touch $@

ovmf-aarch64:
	mkdir -p ovmf-aarch64
	cd ovmf-aarch64 && curl -o OVMF.fd \
		https://retrage.github.io/edk2-nightly/bin/RELEASEAARCH64_QEMU_EFI.fd

ovmf-riscv64:
	mkdir -p ovmf-riscv64
	cd ovmf-riscv64 && curl -o OVMF.fd \
		https://retrage.github.io/edk2-nightly/bin/RELEASERISCV64_VIRT_CODE.fd && \
		dd if=/dev/zero of=OVMF.fd bs=1 count=0 seek=33554432


reconfigure-kernel: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --reconfigure --rebuild kernel-headers kernel)

rebuild-mlibc: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --rebuild mlibc-headers mlibc)

rebuild-posix:  ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --rebuild posix_server)

rebuild-kernel: ${BUILD_SETUP_TARGET}
	rm -f build/m68k/pkg-builds/m68k-virt-loader/lisp.p/loader_Loader.cpp.o
	rm -f build/m68k/pkg-builds/m68k-virt-loader/lisp
	(cd ${BUILD_DIR} && xbstrap install --rebuild kernel-headers kernel ${PLATFORM_EXTRAS})

build-all: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install -c --all)

iso: build-all
	env ARCH=$(ARCH) PLATFORM=$(PLATFORM) tools/mkiso.sh

debug:
	gdb-multiarch -x tools/gdbinit-$(PLATFORM)
