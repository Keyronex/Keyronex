ARCH?=amd64
BUILD_DIR=build/$(ARCH)
SYSROOT_DIR=${BUILD_DIR}/system-root
BUILD_SETUP_TARGET=${BUILD_DIR}/.setup

ifeq ($(ARCH), amd64)
	TARGET_TRIPLE := x86_64-keyronex
else ifeq ($(ARCH), m68k)
	TARGET_TRIPLE := m68k-keyronex
else ifeq ($(ARCH), aarch64)
	TARGET_TRIPLE := aarch64-keyronex
else ifeq ($(ARCH), riscv64)
	TARGET_TRIPLE := riscv64-keyronex
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
	echo "    match:" >> ${BUILD_DIR}/bootstrap-site.yml
	echo "      - ${ARCH}" >> ${BUILD_DIR}/bootstrap-site.yml
	(cd ${BUILD_DIR} && xbstrap init ../..)
	touch $@

rebuild-kernel: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install --rebuild kernel-headers kernel)

build-all: ${BUILD_SETUP_TARGET}
	(cd ${BUILD_DIR} && xbstrap install -c --all)