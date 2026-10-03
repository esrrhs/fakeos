# ==============================================================================
# fakeos - Makefile
# ==============================================================================

# Toolchain configuration (Auto-detect cross-compiler or native Linux/macOS tools)
FCC       ?= $(shell which fakecc 2>/dev/null || echo fakecc)
NASM      ?= $(shell which nasm 2>/dev/null || echo nasm)
QEMU      ?= $(shell which qemu-system-x86_64 2>/dev/null || echo qemu-system-x86_64)

ifeq ($(origin LD),default)
LD        := $(shell which x86_64-elf-ld 2>/dev/null || which x86_64-linux-gnu-ld 2>/dev/null || echo ld)
else
LD        ?= $(shell which x86_64-elf-ld 2>/dev/null || which x86_64-linux-gnu-ld 2>/dev/null || echo ld)
endif

ifeq ($(origin OBJCOPY),default)
OBJCOPY   := $(shell which x86_64-elf-objcopy 2>/dev/null || which x86_64-linux-gnu-objcopy 2>/dev/null || echo objcopy)
else
OBJCOPY   ?= $(shell which x86_64-elf-objcopy 2>/dev/null || which x86_64-linux-gnu-objcopy 2>/dev/null || echo objcopy)
endif

# Project directories
BUILD_DIR := build
ROOT_DIR  := $(shell pwd)
# External fakecc source tree used to build the in-OS native compiler
# (milestone 5). Never modified by this build; tools/m5_build_fakecc.sh works
# in a throwaway /tmp copy.
FAKECC_SRC ?= $(ROOT_DIR)/../fakecc

# Milestone 5 staging tree (in-OS compiler, kernel sources, reference objects).
M5STAGE := $(BUILD_DIR)/m5stage
M5ASM   := $(BUILD_DIR)/m5blob.asm
M5OBJ   := $(BUILD_DIR)/m5blob.o

# Flags
NASMFLAGS := -f elf64
# -O0 keeps locals in memory: fakecc's -O1 SSA register promotion currently
# mis-handles some long-lived pointer locals across va_arg extraction (the
# format pointer in kprintf was observed coalesced onto a scratch register).
# Revisit -O1 once the allocator interference bug is fixed upstream.
# --target=x86_64-linux: this fakecc build ships no arm64-macos backend, and
# the host default on Apple Silicon would otherwise be selected and rejected.
FCCFLAGS  := -O0 --target=x86_64-linux
LDFLAGS   := -T boot/linker.ld -n --gc-sections
QEMUFLAGS := -m 128M -serial stdio

# Target ELF files
KERNEL_64  := $(BUILD_DIR)/fakeos64.elf
KERNEL_BIN := $(BUILD_DIR)/fakeos.elf

# Object files
OBJS := \
	$(BUILD_DIR)/boot.o \
	$(BUILD_DIR)/io.o \
	$(BUILD_DIR)/desc.o \
	$(BUILD_DIR)/switch.o \
	$(BUILD_DIR)/sysentry.o \
	$(BUILD_DIR)/types.o \
	$(BUILD_DIR)/arch.o \
	$(BUILD_DIR)/gdt.o \
	$(BUILD_DIR)/idt.o \
	$(BUILD_DIR)/lapic.o \
	$(BUILD_DIR)/uart.o \
	$(BUILD_DIR)/vga.o \
	$(BUILD_DIR)/kprintf.o \
	$(BUILD_DIR)/isr.o \
	$(BUILD_DIR)/pmm.o \
	$(BUILD_DIR)/vmm.o \
	$(BUILD_DIR)/slab.o \
	$(BUILD_DIR)/as.o \
	$(BUILD_DIR)/ramfs.o \
	$(BUILD_DIR)/memtest.o \
	$(BUILD_DIR)/sched.o \
	$(BUILD_DIR)/proctest.o \
	$(BUILD_DIR)/syscall.o \
	$(BUILD_DIR)/sysfile.o \
	$(BUILD_DIR)/sysproc.o \
	$(BUILD_DIR)/proc.o \
	$(BUILD_DIR)/tty.o \
	$(BUILD_DIR)/uproc.o \
	$(BUILD_DIR)/rootfs.o \
	$(BUILD_DIR)/m5blob.o \
	$(BUILD_DIR)/vfstest.o \
	$(BUILD_DIR)/userblob.o \
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

$(BUILD_DIR)/desc.o: arch/x86_64/desc.asm
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD_DIR)/switch.o: arch/x86_64/switch.asm
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD_DIR)/sysentry.o: arch/x86_64/sysentry.asm
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

# Userland: fakecc-compiled programs linked at 0x400000, embedded into the
# kernel via incbin. Every program shares crt0.o, libc.o and usys.o.
USER_PROGS := init sh hello cat ls kbuild ldfake dumpbin
USER_OBJS  := $(addprefix $(BUILD_DIR)/user/,crt0.o usys.o libc.o) \
              $(foreach p,$(USER_PROGS),$(BUILD_DIR)/user/$(p).o)
USER_ELFS  := $(foreach p,$(USER_PROGS),$(BUILD_DIR)/user/$(p).elf)

$(BUILD_DIR)/user/crt0.o: user/crt0.asm
	@mkdir -p $(BUILD_DIR)/user
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD_DIR)/user/usys.o: user/usys.asm
	@mkdir -p $(BUILD_DIR)/user
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD_DIR)/user/libc.o: user/libc.c
	@mkdir -p $(BUILD_DIR)/user
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/user/%.o: user/%.c
	@mkdir -p $(BUILD_DIR)/user
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

# Each program ELF: crt0 + syscall wrappers + libc + program main.
define USER_ELF_RULE
$(BUILD_DIR)/user/$(1).elf: $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/usys.o \
                           $(BUILD_DIR)/user/libc.o $(BUILD_DIR)/user/$(1).o user/user.ld
	@echo "  [LD]      $$@"
	@$(LD) -T user/user.ld -n -o $$@ $(BUILD_DIR)/user/crt0.o $(BUILD_DIR)/user/usys.o \
	       $(BUILD_DIR)/user/libc.o $(BUILD_DIR)/user/$(1).o
endef
$(foreach p,$(USER_PROGS),$(eval $(call USER_ELF_RULE,$(p))))

$(BUILD_DIR)/userblob.o: user/blob.asm $(USER_ELFS) user/motd.txt
	@mkdir -p $(BUILD_DIR)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

# --- Milestone 5: in-OS compiler/source/reference/reference staging -------------------
M5_REF_DEPS := $(filter-out $(BUILD_DIR)/userblob.o $(M5OBJ),$(OBJS))

# m5blob incbins the staged tree AND the current userblob (the rebuilt
# kernel must keep an equivalent userspace); userblob must be rebuilt first.
$(M5ASM): $(M5_REF_DEPS) $(BUILD_DIR)/userblob.o user/progs/ping.c \
	tools/m5_stage.sh tools/m5_build_fakecc.sh
	@mkdir -p $(M5STAGE)
	@FAKECC_SRC=$(FAKECC_SRC) bash tools/m5_stage.sh $(M5STAGE)

$(M5OBJ): $(M5ASM)
	@echo "  [NASM]    $<"
	@$(NASM) $(NASMFLAGS) -o $@ $<

# C objects compiled with fakecc
$(BUILD_DIR)/types.o: types/types.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/arch.o: arch/arch.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: arch/gdt.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/idt.o: arch/idt.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/lapic.o: arch/lapic.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/uart.o: drivers/uart.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/vga.o: drivers/vga.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/kprintf.o: kernel/kprintf.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/isr.o: kernel/isr.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/pmm.o: mem/pmm.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/vmm.o: mem/vmm.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/slab.o: mem/slab.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/as.o: mem/as.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/ramfs.o: fs/ramfs.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/sched.o: kernel/sched.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/proctest.o: kernel/proctest.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/syscall.o: kernel/syscall.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/sysfile.o: kernel/sysfile.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/tty.o: kernel/tty.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/sysproc.o: kernel/sysproc.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/proc.o: kernel/proc.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/uproc.o: kernel/uproc.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/rootfs.o: kernel/rootfs.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/vfstest.o: kernel/vfstest.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/memtest.o: kernel/memtest.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

$(BUILD_DIR)/kmain.o: kernel/kmain.c
	@mkdir -p $(BUILD_DIR)
	@echo "  [FAKECC]  $<"
	@FAKECC_PKG=$(ROOT_DIR) $(FCC) $(FCCFLAGS) -c $< -o $@

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
