ARCH?=amd64
GDB?=gdb-multiarch
BUILD_DIR=build/$(ARCH)
BUILD_SETUP_TARGET=${BUILD_DIR}/.setup

ifeq ($(ARCH), amd64)
	ARCH=amd64
	TARGET_TRIPLE=x86_64-keyronex
else ifeq ($(ARCH), m68k)
	ARCH=m68k
	TARGET_TRIPLE=m68k-keyronex
	ARCH_EXTRAS=m68k-virt-loader
else ifeq ($(ARCH), riscv64)
	ARCH=riscv64
	TARGET_TRIPLE=riscv64-keyronex
else ifeq($(ARCH), aarch64)
	ARCH=aarch64
	TARGET_TRIPLE=aarch64-keyronex
else
	$(error Unsupported ARCH: $(ARCH))
endif

all: build-all

setup-env:
	rm -f .clangd
	ln -s tools/.clangd-${ARCH} .clangd

ovmf-riscv64:
	mkdir -p ovmf-riscv64
	cd ovmf-riscv64 && curl -o OVMF.fd \
		https://retrage.github.io/edk2-nightly/bin/RELEASERISCV64_VIRT_CODE.fd && \
		dd if=/dev/zero of=OVMF.fd bs=1 count=0 seek=33554432

ovmf-aarch64:
	mkdir -p ovmf-aarch64
	cd ovmf-aarch64 && curl -o OVMF.fd \
		https://retrage.github.io/edk2-nightly/bin/RELEASEAARCH64_QEMU_EFI.fd

# create the build/arch directory and set it up
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

do-build: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install -c --all)

iso:
ifneq ($(ARCH), m68k)
	tools/mkiso.sh -a $(ARCH)
endif

reconfigure-kernel: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --reconfigure --rebuild kernel ${ARCH_EXTRAS})

rebuild-kernel: ${BUILD_SETUP_TARGET}
ifeq ($(ARCH), m68k)
	rm -f build/m68k/pkg-builds/m68k-virt-loader/lisp.p/loader_Loader.cpp.o
	rm -f build/m68k/pkg-builds/m68k-virt-loader/lisp
endif
	(cd ${BUILD_DIR} && xbstrap install --rebuild kernel ${ARCH_EXTRAS})

rebuild-mlibc: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --rebuild mlibc-headers mlibc)

build-core: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install kernel coreutils bash init)

build-all: do-build iso

run:
	tools/run.sh -a $(ARCH)

debug:
	$(GDB) -x tools/gdbinit-$(ARCH)
