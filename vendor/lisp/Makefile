KERNEL_BLOB =
export X_CXX_BIN =
export X_LD_BIN =
export X_AS_BIN =

export BUILD_DIR = build
export LOADER_FULL_FILEPATH = $(abspath loader/$(BUILD_DIR)/loader-m68k.elf)

.PHONY: all
all: $(KERNEL_BLOB)
	@-rm -f loader/$(BUILD_DIR)/kernel.elf
	@mkdir -p loader/$(BUILD_DIR)
	@cp $(KERNEL_BLOB) loader/$(BUILD_DIR)/kernel.elf
	@cd loader; $(MAKE) all --no-print-directory
	@printf "Loader built: $(LOADER_FULL_FILEPATH)\r\n"

.PHONY: clean
clean:
	@-cd loader; $(MAKE) clean --no-print-directory

.PHONY: bootstrap
bootstrap:
	@-mkdir -p thirdparty
	@curl -o thirdparty/Limine.h https://raw.githubusercontent.com/limine-bootloader/limine/v8.x/limine.h
	@curl -o thirdparty/NanoPrintf.h https://raw.githubusercontent.com/charlesnicholson/nanoprintf/v0.5.3/nanoprintf.h
	@patch thirdparty/Limine.h limine-h.patch
