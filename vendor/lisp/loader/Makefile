LOADER_CXX_FLAGS = -Wall -Wextra -fstack-protector-strong -ffreestanding -std=c++17 \
	-fno-rtti -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -g \
	-m68040 -I. -I../lib -I../thirdparty \
	-DNPL_ENABLE_LOGGING
LOADER_LD_FLAGS = -nostdlib -static --no-dynamic-linker

CXX_SRCS = Main.cpp Loader.cpp Memman.cpp Syslib.cpp Util.cpp
AS_SRCS = Entry.S

LD_SCRIPT = Linker.lds
CXX_OBJS = $(patsubst %.cpp, $(BUILD_DIR)/%.cpp.o, $(CXX_SRCS))
AS_OBJS = $(patsubst %.S, $(BUILD_DIR)/%.S.o, $(AS_SRCS))
OBJS = $(AS_OBJS) $(CXX_OBJS)

.PHONY: all
all: $(LOADER_FULL_FILEPATH)

$(LOADER_FULL_FILEPATH): $(OBJS) $(LD_SCRIPT)
	@printf "Linking ...\r\n"
	$(X_LD_BIN) $(OBJS) $(LOADER_LD_FLAGS) -T $(LD_SCRIPT) -o $(LOADER_FULL_FILEPATH)
	@printf "Done.\r\n"

.PHONY: clean
clean:
	@printf "Cleaning build files ...\r\n"
	-rm -r $(BUILD_DIR)
	@printf "Done.\r\n"

$(BUILD_DIR)/%.S.o: %.S
	@printf "Assembling: $<\r\n"
	@mkdir -p $(@D)
	$(X_AS_BIN) $(LOADER_AS_FLAGS) $< -o $@

$(BUILD_DIR)/%.cpp.o: %.cpp
	@printf "Compiling: $<\r\n"
	@mkdir -p $(@D)
	$(X_CXX_BIN) $(LOADER_CXX_FLAGS) -c $< -o $@

$(BUILD_DIR)/Loader.cpp.o: Loader.cpp $(BUILD_DIR)/kernel.elf
	@printf "Compiling (with embedded kernel): $<\r\n"
	@mkdir -p $(@D)
	$(X_CXX_BIN) $(LOADER_CXX_FLAGS) -c $< -o $@

