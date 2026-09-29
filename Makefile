# ==============================================================================
# fakeos - Makefile
# ==============================================================================

# Toolchain configuration
FCC       := fakecc
NASM      := nasm
LD        := x86_64-elf-ld
OBJCOPY   := x86_64-elf-objcopy
QEMU      := qemu-system-x86_64

# Project directories
BUILD_DIR := build
ROOT_DIR  := $(shell pwd)

# Flags
NASMFLAGS := -f elf64
LDFLAGS   := -T boot/linker.ld -n --gc-sections
QEMUFLAGS := -m 128M -serial stdio

# Target ELF files
KERNEL_64  := $(BUILD_DIR)/fakeos64.elf
KERNEL_BIN := $(BUILD_DIR)/fakeos.elf

# Object files
OBJS := \
	$(BUILD_DIR)/boot.o \
	$(BUILD_DIR)/io.o \
	$(BUILD_DIR)/types.o \
	$(BUILD_DIR)/arch.o \
	$(BUILD_DIR)/uart.o \
	$(BUILD_DIR)/vga.o \
	$(BUILD_DIR)/kprintf.o \
	$(BUILD_DIR)/kmain.o

.PHONY: all build run qemu qemu-gui clean help

all: build

build: $(KERNEL_BIN)

# Link 64-bit Higher-Half ELF kernel
$(KERNEL_64): $(OBJS) boot/linker.ld
	@mkdir -p $(BUILD_DIR)
	@echo "  [LD]      $@"
	@$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Convert to ELF32 multiboot image for direct QEMU loading
$(KERNEL_BIN): $(KERNEL_64)
	@echo "  [OBJCOPY] $@"
	@$(OBJCOPY) -O elf32-i386 $< $@
	@echo "=================================================="
	@echo " fakeos kernel build completed: $@"
	@echo "=================================================="

# Assembly objects
$(BUILD_DIR)/boot.o: arch/x86_64/boot.asm
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD_DIR)/io.o: arch/x86_64/io.asm
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

# C objects compiled with fakecc
$(BUILD_DIR)/types.o: types/types.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

$(BUILD_DIR)/arch.o: arch/arch.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

$(BUILD_DIR)/uart.o: drivers/uart.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

$(BUILD_DIR)/vga.o: drivers/vga.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

$(BUILD_DIR)/kprintf.o: kernel/kprintf.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

$(BUILD_DIR)/kmain.o: kernel/kmain.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) -c $< -o $@

# Run automated test
test: build
	@python3 scripts/test_qemu.py

# Run in QEMU (Headless terminal mode: standard I/O hooked to UART serial port)
qemu run: build
	@echo "Launching fakeos in QEMU (Press Ctrl-A then X to exit)..."
	@$(QEMU) -kernel $(KERNEL_BIN) $(QEMUFLAGS) -display none -no-reboot

# Run in QEMU with graphical VGA window
qemu-gui: build
	@echo "Launching fakeos in QEMU graphical window..."
	@$(QEMU) -kernel $(KERNEL_BIN) $(QEMUFLAGS)

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR)
