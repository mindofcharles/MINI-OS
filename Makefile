BUILD_DIR := build
SRC_DIR := OS_src
BOOT_SRC := $(SRC_DIR)/boot/boot.asm
KERNEL_SRC := $(SRC_DIR)/kernel/main.asm
KERNEL_DEPS := $(shell find $(SRC_DIR)/kernel -type f \( -name '*.asm' -o -name '*.def' \) | sort) transport/lib/syscall.def transport/lib/net/raw.def
FS_LAYOUT_DEF := $(SRC_DIR)/kernel/fs/layout.def
PLATFORM_LAYOUT_DEF := $(SRC_DIR)/kernel/platform_layout.def
APP_LINKER_SCRIPT := transport/app.ld
FS_DATA_START_LBA := $(shell grep FS_DATA_START_LBA $(FS_LAYOUT_DEF) | tr -cd '0-9\n')
FS_DATA_BLOCK_COUNT := $(shell grep FS_DATA_BLOCK_COUNT $(FS_LAYOUT_DEF) | tr -cd '0-9\n')
OS_SECTORS := $(shell expr $(FS_DATA_START_LBA) + $(FS_DATA_BLOCK_COUNT))

layout_value = $(strip $(shell awk -F '[(),[:space:]]+' '$$2 == "$(1)" { print $$3 }' $(PLATFORM_LAYOUT_DEF)))
APP_IMAGE_BASE := $(call layout_value,APP_IMAGE_BASE)
APP_IMAGE_SIZE := $(call layout_value,APP_IMAGE_SIZE)
KERNEL_IMAGE_MAX_SIZE := $(call layout_value,KERNEL_IMAGE_MAX_SIZE)
PLATFORM_CONFIGURED_MEMORY_BYTES := $(call layout_value,PLATFORM_CONFIGURED_MEMORY_BYTES)
MAX_APP_IMAGE_SIZE := $(shell printf '%d' $(APP_IMAGE_SIZE))
KERNEL_MAX_SECTORS := $(shell printf '%d' $$(( $(KERNEL_IMAGE_MAX_SIZE) / 512 )))
QEMU_MEMORY_MB := $(shell printf '%d' $$(( $(PLATFORM_CONFIGURED_MEMORY_BYTES) / 1048576 )))

BOOT_BIN := $(BUILD_DIR)/boot.bin
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
OS_IMG := $(BUILD_DIR)/mini_os.img

INJECT_TOOL := $(BUILD_DIR)/inject_transport
ELF2BIN_TOOL := $(BUILD_DIR)/elf2bin
CHECK_TOOL := $(BUILD_DIR)/check_image
LAYOUT_TOOL := $(BUILD_DIR)/check_layout
CHECK_SOURCES := tools/check_image.c tools/check_image.h

LIB_DIR := transport/lib
APPS_DIR := transport/apps
LIB_TEST_DIR := transport/lib_test
APP_BUILD_DIR := transport/build
APP_OBJ_DIR := $(BUILD_DIR)/transport
TRANSPORT_MANIFEST_TOOL := tools/transport_manifest.sh
TRANSPORT_INJECT_MANIFEST := $(BUILD_DIR)/transport-inputs.manifest

CRT0_SRC := $(LIB_DIR)/crt0.asm
MINILIBC_SRC := $(LIB_DIR)/minilibc.c
COMPILER_RT_SRC := $(LIB_DIR)/compiler_rt.c
LIB_HEADERS := $(shell find $(LIB_DIR) -type f -name '*.h' | sort) $(LIB_DIR)/syscall.def $(LIB_DIR)/net/raw.def

CRT0_OBJ := $(BUILD_DIR)/crt0.o
MINILIBC_OBJ := $(BUILD_DIR)/minilibc.o
COMPILER_RT_OBJ := $(BUILD_DIR)/compiler_rt.o

NET_BASE_LIB_SRCS := $(addprefix $(LIB_DIR)/net/,raw.c platform.c time.c)
NET_PHASE_D2_LIB_SRCS := $(addprefix $(LIB_DIR)/net/,net.c address.c byteorder.c checksum.c)
NET_IPV4_LIB_SRCS := $(NET_PHASE_D2_LIB_SRCS) $(LIB_DIR)/net/ethernet.c
NET_ALL_LIB_SRCS := $(shell find $(LIB_DIR)/net -type f -name '*.c' 2>/dev/null | sort)
NET_GROUPED_LIB_SRCS := $(NET_BASE_LIB_SRCS) $(NET_IPV4_LIB_SRCS)
duplicate_words = $(sort $(foreach item,$(1),$(if $(word 2,$(filter $(item),$(1))),$(item))))
NET_DUPLICATE_LIB_SRCS := $(call duplicate_words,$(NET_GROUPED_LIB_SRCS))
NET_UNGROUPED_LIB_SRCS := $(filter-out $(NET_GROUPED_LIB_SRCS),$(NET_ALL_LIB_SRCS))
NET_MISSING_LIB_SRCS := $(filter-out $(NET_ALL_LIB_SRCS),$(NET_GROUPED_LIB_SRCS))

ifneq ($(strip $(NET_DUPLICATE_LIB_SRCS)),)
$(error network library sources assigned to multiple groups: $(NET_DUPLICATE_LIB_SRCS))
endif
ifneq ($(strip $(NET_UNGROUPED_LIB_SRCS)),)
$(error network library sources have no object group: $(NET_UNGROUPED_LIB_SRCS))
endif
ifneq ($(strip $(NET_MISSING_LIB_SRCS)),)
$(error grouped network library sources do not exist: $(NET_MISSING_LIB_SRCS))
endif

SSH_LIB_SRCS := $(shell find $(LIB_DIR)/ssh -type f -name '*.c' 2>/dev/null | sort)
NET_BASE_LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(APP_OBJ_DIR)/lib/%.o,$(NET_BASE_LIB_SRCS))
NET_PHASE_D2_LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(APP_OBJ_DIR)/lib/%.o,$(NET_PHASE_D2_LIB_SRCS))
NET_IPV4_LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(APP_OBJ_DIR)/lib/%.o,$(NET_IPV4_LIB_SRCS))
SSH_LIB_OBJS := $(patsubst $(LIB_DIR)/%.c,$(APP_OBJ_DIR)/lib/%.o,$(SSH_LIB_SRCS))
NETWORK_BASE_APP_NAMES := netdiag ping netcat ssh test_network
NETWORK_IPV4_APP_NAMES := ping netcat ssh
SSH_APP_NAMES := ssh

app_component_objects = $(strip $(if $(filter $(NETWORK_BASE_APP_NAMES),$(notdir $(1))),$(COMPILER_RT_OBJ) $(NET_BASE_LIB_OBJS)) $(if $(filter $(NETWORK_IPV4_APP_NAMES),$(notdir $(1))),$(NET_IPV4_LIB_OBJS)) $(if $(filter $(SSH_APP_NAMES),$(notdir $(1))),$(SSH_LIB_OBJS)))

ifneq ($(strip $(NET_BASE_LIB_OBJS) $(NET_IPV4_LIB_OBJS) $(SSH_LIB_OBJS)),)
.SECONDARY: $(NET_BASE_LIB_OBJS) $(NET_IPV4_LIB_OBJS) $(SSH_LIB_OBJS)
endif

APP_SRCS := $(shell find $(APPS_DIR) $(LIB_TEST_DIR) -name '*.c' 2>/dev/null | sort)
APP_BINS := $(patsubst transport/%.c,$(APP_BUILD_DIR)/%.bin,$(APP_SRCS))

NASM := nasm -w-label-redef-late

CC := cc
CLANG := clang
LLD := ld.lld
QEMU := qemu-system-i386
PYTHON := python3

NETWORK_PHASE0_TOOL := tools/network_phase0/build.sh
NETWORK_PHASE0_HOST_TOOL := tools/network_phase0/host/build.sh
NETWORK_PHASE0_OUTPUT ?= $(BUILD_DIR)/network-phase0
LIBSSH2_SOURCE ?=
MBEDTLS_SOURCE ?=
WOLFSSH_SOURCE ?=
WOLFSSL_SOURCE ?=
NETWORK_PHASE_B_TEST_BIN := $(BUILD_DIR)/network-phase-b/platform_test
NETWORK_PHASE_B_TEST_SRCS := tests/network_phase_b/test_platform.c tests/network_phase_b/deterministic_platform.c $(LIB_DIR)/net/time.c
NETWORK_PHASE_C_ABI_TEST_BIN := $(BUILD_DIR)/network-phase-c/raw_abi_test
NETWORK_PHASE_C_ABI_TEST_SRC := tests/network_phase_c/test_raw_abi.c
NETWORK_PHASE_D_PUBLIC_HEADER_OBJ := $(BUILD_DIR)/network-phase-d/public_header.o
NETWORK_PHASE_D_BACKEND_TEST_BIN := $(BUILD_DIR)/network-phase-d/backend_test
NETWORK_PHASE_D_BACKEND_TEST_SRCS := tests/network_phase_d/test_backend.c tests/network_phase_d/deterministic_backend.c
NETWORK_PHASE_D_PRIMITIVE_TEST_BIN := $(BUILD_DIR)/network-phase-d/primitive_test
NETWORK_PHASE_D_PRIMITIVE_TEST_SRCS := tests/network_phase_d/test_primitives.c $(LIB_DIR)/net/byteorder.c $(LIB_DIR)/net/checksum.c
NETWORK_PHASE_D_ADDRESS_TEST_BIN := $(BUILD_DIR)/network-phase-d/address_test
NETWORK_PHASE_D_ADDRESS_TEST_SRCS := tests/network_phase_d/test_address.c tests/network_phase_d/deterministic_backend.c $(LIB_DIR)/net/address.c $(LIB_DIR)/net/net.c
NETWORK_PHASE_D_ETHERNET_TEST_BIN := $(BUILD_DIR)/network-phase-d/ethernet_test
NETWORK_PHASE_D_ETHERNET_TEST_SRCS := tests/network_phase_d/test_ethernet.c tests/network_phase_d/deterministic_backend.c $(LIB_DIR)/net/net.c $(LIB_DIR)/net/address.c $(LIB_DIR)/net/byteorder.c $(LIB_DIR)/net/ethernet.c

TARGET_CFLAGS := -target i386-unknown-none-elf -m32 -march=i386 -mno-sse -mno-mmx -ffreestanding -nostdlib -O2 -I$(LIB_DIR)
LIB_CFLAGS := $(TARGET_CFLAGS) -std=gnu11
NET_LIB_CFLAGS := $(LIB_CFLAGS) -Wall -Wextra -Werror
APP_CFLAGS := $(TARGET_CFLAGS) -std=c90 -pedantic-errors -Wall -Wextra -Werror
HOST_SYSROOT := $(shell if [ "$$(uname -s)" = Darwin ]; then xcrun --sdk macosx --show-sdk-path 2>/dev/null; fi)
HOST_CFLAGS := -O2 -std=c11 -Wall -Wextra -Werror $(if $(HOST_SYSROOT),-isysroot $(HOST_SYSROOT))
HOST_ENV := $(if $(HOST_SYSROOT),SDKROOT=$(HOST_SYSROOT))
ELF2BIN_MAX_OBJECTS ?= 256
ELF2BIN_MAX_SYMBOLS ?= 4096
ELF2BIN_MAX_SECTIONS ?= 4096
ELF2BIN_MAX_RELOCATIONS ?= 32768
QEMU_MEMORY ?= $(QEMU_MEMORY_MB)M

.PHONY: all clean run run-network apps app check-layout check-image test test-build test-e2e test-network test-network-host test-network-abi test-network-d1 test-network-d2 test-network-d3 test-network-driver test-network-qemu network-phase0-check network-phase0-selected network-phase0 network-phase0-host-probe transport-inputs-force

all: check-layout $(OS_IMG)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

transport-inputs-force:

$(TRANSPORT_INJECT_MANIFEST): transport-inputs-force $(TRANSPORT_MANIFEST_TOOL) | $(BUILD_DIR)
	@sh $(TRANSPORT_MANIFEST_TOOL) transport $(APP_BUILD_DIR) > $@.tmp
	@if [ -f $@ ] && cmp -s $@.tmp $@; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
	fi

$(APP_BUILD_DIR):
	mkdir -p $(APP_BUILD_DIR)

$(INJECT_TOOL): tools/inject_transport.c $(CHECK_SOURCES) $(FS_LAYOUT_DEF) Makefile | $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) -DCHECK_IMAGE_LIBRARY tools/inject_transport.c tools/check_image.c -o $(INJECT_TOOL)

$(ELF2BIN_TOOL): tools/elf2bin.c Makefile | $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) tools/elf2bin.c -o $(ELF2BIN_TOOL)

$(CHECK_TOOL): $(CHECK_SOURCES) $(FS_LAYOUT_DEF) Makefile | $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) tools/check_image.c -o $(CHECK_TOOL)

$(LAYOUT_TOOL): tools/check_layout.c $(PLATFORM_LAYOUT_DEF) $(LIB_DIR)/net/raw.def Makefile | $(BUILD_DIR)
	$(CC) $(HOST_CFLAGS) tools/check_layout.c -o $(LAYOUT_TOOL)

check-layout: $(LAYOUT_TOOL)
	$(LAYOUT_TOOL)

$(CRT0_OBJ): $(CRT0_SRC) Makefile | $(BUILD_DIR)
	$(NASM) -f elf32 $(CRT0_SRC) -o $(CRT0_OBJ)

$(MINILIBC_OBJ): $(MINILIBC_SRC) $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	$(CLANG) $(LIB_CFLAGS) -c $(MINILIBC_SRC) -o $(MINILIBC_OBJ)

$(COMPILER_RT_OBJ): $(COMPILER_RT_SRC) Makefile | $(BUILD_DIR)
	$(CLANG) $(LIB_CFLAGS) -c $(COMPILER_RT_SRC) -o $(COMPILER_RT_OBJ)

$(APP_OBJ_DIR)/lib/net/%.o: $(LIB_DIR)/net/%.c $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(NET_LIB_CFLAGS) -c $< -o $@

$(APP_OBJ_DIR)/lib/%.o: $(LIB_DIR)/%.c $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(LIB_CFLAGS) -c $< -o $@

# Generic rule to compile any C app or library test under transport/
.SECONDEXPANSION:
$(APP_BUILD_DIR)/%.bin: transport/%.c $(LIB_HEADERS) $(CRT0_OBJ) $(MINILIBC_OBJ) $(ELF2BIN_TOOL) $(LAYOUT_TOOL) $(APP_LINKER_SCRIPT) $$(call app_component_objects,$$*) Makefile | $(BUILD_DIR) $(APP_BUILD_DIR)
	@echo "Compiling C binary '$<'..."
	@mkdir -p $(dir $@)
	@mkdir -p $(dir $(APP_OBJ_DIR)/$*.o)
	@$(LAYOUT_TOOL) >/dev/null
	$(CLANG) $(APP_CFLAGS) -c $< -o $(APP_OBJ_DIR)/$*.o
	@if command -v $(LLD) >/dev/null 2>&1; then \
		echo "Linking '$@' with ld.lld..."; \
		$(LLD) -m elf_i386 --orphan-handling=error --defsym=APP_LINK_BASE=$(APP_IMAGE_BASE) --defsym=APP_LINK_SIZE=$(APP_IMAGE_SIZE) -T $(APP_LINKER_SCRIPT) --oformat binary $(CRT0_OBJ) $(APP_OBJ_DIR)/$*.o $(MINILIBC_OBJ) $(call app_component_objects,$*) -o $@; \
	else \
		echo "ld.lld not found; linking '$@' with elf2bin..."; \
		$(ELF2BIN_TOOL) --max-output $(MAX_APP_IMAGE_SIZE) --max-objects $(ELF2BIN_MAX_OBJECTS) --max-symbols $(ELF2BIN_MAX_SYMBOLS) --max-sections $(ELF2BIN_MAX_SECTIONS) --max-relocations $(ELF2BIN_MAX_RELOCATIONS) $@ $(APP_IMAGE_BASE) $(CRT0_OBJ) $(APP_OBJ_DIR)/$*.o $(MINILIBC_OBJ) $(call app_component_objects,$*); \
	fi
	@APP_SIZE=$$(wc -c < $@); \
	if [ $$APP_SIZE -gt $(MAX_APP_IMAGE_SIZE) ]; then \
		echo "error: '$@' is $$APP_SIZE bytes; application images are limited to $(MAX_APP_IMAGE_SIZE) bytes."; \
		rm -f $@; \
		exit 1; \
	fi

apps: $(APP_BINS)

# Single app compile target (usage: make app APP=hello.c or make app APP=hello)
app:
	@if [ -z "$(APP)" ]; then \
		echo "Usage: make app APP=<name.c>"; \
		exit 1; \
	fi; \
	APP_NAME=$$(basename $(APP) .c); \
	APP_SOURCE=$(APPS_DIR)/$$APP_NAME.c; \
	if [ ! -f $$APP_SOURCE ]; then \
		echo "error: application source '$$APP_SOURCE' does not exist."; \
		exit 1; \
	fi; \
	$(MAKE) $(APP_BUILD_DIR)/apps/$$APP_NAME.bin

$(KERNEL_BIN): $(KERNEL_DEPS) $(PLATFORM_LAYOUT_DEF) $(LAYOUT_TOOL) Makefile | $(BUILD_DIR)
	@$(LAYOUT_TOOL) >/dev/null
	$(NASM) -f bin $(KERNEL_SRC) -o $(KERNEL_BIN)

$(BOOT_BIN): $(BOOT_SRC) $(KERNEL_BIN) $(FS_LAYOUT_DEF) $(PLATFORM_LAYOUT_DEF) Makefile | $(BUILD_DIR)
	@KERNEL_SIZE=$$(wc -c < $(KERNEL_BIN)); \
	KERNEL_SECTORS=$$(( (KERNEL_SIZE + 511) / 512 )); \
	if [ $$KERNEL_SECTORS -gt $(KERNEL_MAX_SECTORS) ]; then \
		echo "error: kernel is $$KERNEL_SECTORS sectors, exceeds reserved $(KERNEL_MAX_SECTORS)-sector area (LBA 1-$(KERNEL_MAX_SECTORS))."; \
		exit 1; \
	fi; \
	$(NASM) -f bin -d KERNEL_SECTORS=$$KERNEL_SECTORS $(BOOT_SRC) -o $(BOOT_BIN)

$(OS_IMG): $(BOOT_BIN) $(KERNEL_BIN) $(INJECT_TOOL) $(CHECK_TOOL) $(APP_BINS) $(TRANSPORT_INJECT_MANIFEST) $(FS_LAYOUT_DEF) Makefile | $(BUILD_DIR)
	dd if=/dev/zero of=$(OS_IMG) bs=512 count=$(OS_SECTORS)
	@IMAGE_SIZE=$$(wc -c < $(OS_IMG)); \
	EXPECTED_SIZE=$$(( $(OS_SECTORS) * 512 )); \
	if [ $$IMAGE_SIZE -ne $$EXPECTED_SIZE ]; then \
		echo "error: image is $$IMAGE_SIZE bytes; filesystem geometry requires $$EXPECTED_SIZE bytes."; \
		exit 1; \
	fi
	dd if=$(BOOT_BIN) of=$(OS_IMG) bs=512 seek=0 conv=notrunc
	dd if=$(KERNEL_BIN) of=$(OS_IMG) bs=512 seek=1 conv=notrunc
	$(INJECT_TOOL) $(OS_IMG) transport
	$(CHECK_TOOL) $(OS_IMG)

check-image: $(OS_IMG) $(CHECK_TOOL)
	$(CHECK_TOOL) $(OS_IMG)

network-phase0-check:
	$(HOST_ENV) bash $(NETWORK_PHASE0_TOOL) --check-manifest

network-phase0-selected: network-phase0-check
	@if [ -z "$(LIBSSH2_SOURCE)" ] || [ -z "$(MBEDTLS_SOURCE)" ]; then \
		echo "Usage: make network-phase0-selected LIBSSH2_SOURCE=/path/to/libssh2 MBEDTLS_SOURCE=/path/to/mbedtls"; \
		exit 1; \
	fi
	$(HOST_ENV) bash $(NETWORK_PHASE0_TOOL) selected "$(LIBSSH2_SOURCE)" "$(MBEDTLS_SOURCE)" "$(NETWORK_PHASE0_OUTPUT)"

network-phase0: network-phase0-check
	@if [ -z "$(LIBSSH2_SOURCE)" ] || [ -z "$(MBEDTLS_SOURCE)" ] || [ -z "$(WOLFSSH_SOURCE)" ] || [ -z "$(WOLFSSL_SOURCE)" ]; then \
		echo "Usage: make network-phase0 LIBSSH2_SOURCE=/path/to/libssh2 MBEDTLS_SOURCE=/path/to/mbedtls WOLFSSH_SOURCE=/path/to/wolfssh WOLFSSL_SOURCE=/path/to/wolfssl"; \
		exit 1; \
	fi
	$(HOST_ENV) bash $(NETWORK_PHASE0_TOOL) all "$(LIBSSH2_SOURCE)" "$(MBEDTLS_SOURCE)" "$(WOLFSSH_SOURCE)" "$(WOLFSSL_SOURCE)" "$(NETWORK_PHASE0_OUTPUT)"

network-phase0-host-probe: network-phase0-check
	@if [ -z "$(LIBSSH2_SOURCE)" ] || [ -z "$(MBEDTLS_SOURCE)" ]; then \
		echo "Usage: make network-phase0-host-probe LIBSSH2_SOURCE=/path/to/libssh2 MBEDTLS_SOURCE=/path/to/mbedtls"; \
		exit 1; \
	fi
	$(HOST_ENV) bash $(NETWORK_PHASE0_HOST_TOOL) "$(LIBSSH2_SOURCE)" "$(MBEDTLS_SOURCE)" "$(NETWORK_PHASE0_OUTPUT)"

test-build: network-phase0-check
	$(HOST_ENV) $(PYTHON) tests/test_build.py

test-e2e: $(OS_IMG) $(CHECK_TOOL)
	$(PYTHON) tests/qemu_e2e.py --image $(OS_IMG) --checker $(CHECK_TOOL)

$(NETWORK_PHASE_B_TEST_BIN): $(NETWORK_PHASE_B_TEST_SRCS) $(LIB_DIR)/net/net_platform.h tests/network_phase_b/deterministic_platform.h Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(NETWORK_PHASE_B_TEST_SRCS) -o $@

$(NETWORK_PHASE_D_PUBLIC_HEADER_OBJ): tests/network_phase_d/test_public_header.c $(LIB_DIR)/net/net.h Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(APP_CFLAGS) -c $< -o $@

$(NETWORK_PHASE_D_BACKEND_TEST_BIN): $(NETWORK_PHASE_D_BACKEND_TEST_SRCS) tests/network_phase_d/deterministic_backend.h $(LIB_DIR)/net/net.h $(LIB_DIR)/net/internal.h $(LIB_DIR)/net/raw.h $(LIB_DIR)/net/net_platform.h Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(NETWORK_PHASE_D_BACKEND_TEST_SRCS) -o $@

$(NETWORK_PHASE_D_PRIMITIVE_TEST_BIN): $(NETWORK_PHASE_D_PRIMITIVE_TEST_SRCS) $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(NETWORK_PHASE_D_PRIMITIVE_TEST_SRCS) -o $@

$(NETWORK_PHASE_D_ADDRESS_TEST_BIN): $(NETWORK_PHASE_D_ADDRESS_TEST_SRCS) tests/network_phase_d/deterministic_backend.h $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(NETWORK_PHASE_D_ADDRESS_TEST_SRCS) -o $@

$(NETWORK_PHASE_D_ETHERNET_TEST_BIN): $(NETWORK_PHASE_D_ETHERNET_TEST_SRCS) tests/network_phase_d/deterministic_backend.h $(LIB_HEADERS) Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) $(NETWORK_PHASE_D_ETHERNET_TEST_SRCS) -o $@

test-network-d1: $(NETWORK_PHASE_D_PUBLIC_HEADER_OBJ) $(NETWORK_PHASE_D_BACKEND_TEST_BIN) $(APP_OBJ_DIR)/lib/net/net.o
	$(NETWORK_PHASE_D_BACKEND_TEST_BIN)

test-network-d2: $(NETWORK_PHASE_D_PUBLIC_HEADER_OBJ) $(NETWORK_PHASE_D_PRIMITIVE_TEST_BIN) $(NETWORK_PHASE_D_ADDRESS_TEST_BIN) $(NET_PHASE_D2_LIB_OBJS)
	$(NETWORK_PHASE_D_PRIMITIVE_TEST_BIN)
	$(NETWORK_PHASE_D_ADDRESS_TEST_BIN)

test-network-d3: $(NETWORK_PHASE_D_ETHERNET_TEST_BIN) $(NET_IPV4_LIB_OBJS)
	$(NETWORK_PHASE_D_ETHERNET_TEST_BIN)

test-network-host: $(NETWORK_PHASE_B_TEST_BIN) $(NETWORK_PHASE_D_PUBLIC_HEADER_OBJ) $(NETWORK_PHASE_D_BACKEND_TEST_BIN) $(NETWORK_PHASE_D_PRIMITIVE_TEST_BIN) $(NETWORK_PHASE_D_ADDRESS_TEST_BIN) $(NETWORK_PHASE_D_ETHERNET_TEST_BIN) $(NET_IPV4_LIB_OBJS)
	$(NETWORK_PHASE_B_TEST_BIN)
	$(NETWORK_PHASE_D_BACKEND_TEST_BIN)
	$(NETWORK_PHASE_D_PRIMITIVE_TEST_BIN)
	$(NETWORK_PHASE_D_ADDRESS_TEST_BIN)
	$(NETWORK_PHASE_D_ETHERNET_TEST_BIN)

$(NETWORK_PHASE_C_ABI_TEST_BIN): $(NETWORK_PHASE_C_ABI_TEST_SRC) $(LIB_DIR)/net/raw.h $(LIB_DIR)/net/raw.def $(LIB_DIR)/syscall.def Makefile | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(HOST_CFLAGS) -std=c90 -pedantic-errors -I$(LIB_DIR) $(NETWORK_PHASE_C_ABI_TEST_SRC) -o $@

test-network-abi: $(NETWORK_PHASE_C_ABI_TEST_BIN)
	$(NETWORK_PHASE_C_ABI_TEST_BIN)

test-network-driver: $(OS_IMG) $(CHECK_TOOL)
	$(PYTHON) tests/network_phase_c.py --image $(OS_IMG)

test-network-qemu: test-network-driver test-e2e

test-network: test-network-host test-network-abi test-network-qemu

test: test-network test-build

run: $(OS_IMG)
	$(QEMU) -m $(QEMU_MEMORY) -drive file=$(OS_IMG),format=raw,if=ide,index=0,media=disk

run-network: $(OS_IMG)
	$(QEMU) -m $(QEMU_MEMORY) -accel tcg -cpu max,rdrand=on -drive file=$(OS_IMG),format=raw,if=ide,index=0,media=disk -netdev user,id=net0 -device ne2k_isa,netdev=net0,iobase=0x300,irq=9,mac=52:54:00:12:34:56

clean:
	rm -rf $(BUILD_DIR) $(APP_BUILD_DIR)
