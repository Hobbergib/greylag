# goose-microkernel
#
#   make            build/kernel32.elf: the Goose core, the x86_64 arch layer, the demo
#   make run        boot it in QEMU, console on stdio (Ctrl-A X to quit)
#   make test       boot it headless and check the demo's output
#   make check      typecheck the Goose core only
#   make archtest   the arch layer alone, under a small C stand-in core (no goose needed)
#
# Needs: goose (https://github.com/aardappel/goose) on PATH or GOOSE=...,
# gcc, GNU ld/objcopy, and qemu-system-x86_64 to run.

GOOSE   ?= goose
CC      ?= gcc
LD      ?= ld
OBJCOPY ?= objcopy
QEMU    ?= qemu-system-x86_64

ARCH  := arch/x86_64
BUILD := build

# Freestanding, no red zone, and no SSE: the kernel never saves the user
# threads' vector registers, so it must not use them itself.
KFLAGS := -O2 -g -std=gnu11 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -fno-asynchronous-unwind-tables -mno-red-zone -mgeneral-regs-only \
          -fno-omit-frame-pointer
CFLAGS_ARCH := $(KFLAGS) -Wall -Wextra -I$(ARCH)
# The generated C: the same machine flags; its warnings are the compiler's business.
CFLAGS_GEN  := $(KFLAGS) -I$(ARCH) -w
# User code must not depend on anything outside the user region.
CFLAGS_USER := -O2 -g -std=gnu11 -ffreestanding -fno-builtin -fno-pic -fno-pie \
               -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone \
               -mgeneral-regs-only -fno-tree-loop-distribute-patterns -Wall -Wextra -Iuser
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack --no-warn-rwx-segments \
           -T $(ARCH)/linker.ld

GOOSE_FLAGS := --stack-reserve 64K -D GS_VEC=0 -D GS_SIMD=0

KERNEL_SRC := $(wildcard kernel/*.goose)
ARCH_OBJS  := $(BUILD)/boot.o $(BUILD)/entry.o $(BUILD)/arch.o $(BUILD)/klibc.o

.PHONY: all run test check archtest run-archtest clean

all: $(BUILD)/kernel32.elf

$(BUILD):
	mkdir -p $(BUILD) $(BUILD)/archtest

# ---- the Goose core ----

$(BUILD)/kernel.c: $(KERNEL_SRC) $(ARCH)/goose_glue.h | $(BUILD)
	$(GOOSE) $(GOOSE_FLAGS) --include $(ARCH)/goose_glue.h --header $(BUILD)/goose_api.h \
	    -o $@ kernel/kernel.goose

$(BUILD)/kernel.o: $(BUILD)/kernel.c $(ARCH)/hal.h
	$(CC) $(CFLAGS_GEN) -c $< -o $@

check:
	$(GOOSE) check $(GOOSE_FLAGS) kernel/kernel.goose

# ---- the arch layer ----

$(BUILD)/%.o: $(ARCH)/%.S $(ARCH)/hal.h | $(BUILD)
	$(CC) $(CFLAGS_ARCH) -c $< -o $@

$(BUILD)/%.o: $(ARCH)/%.c $(ARCH)/hal.h $(ARCH)/kernel_api.h | $(BUILD)
	$(CC) $(CFLAGS_ARCH) -c $< -o $@

# ---- user space ----

$(BUILD)/user.o: user/user.c user/syscall.h | $(BUILD)
	$(CC) $(CFLAGS_USER) -c $< -o $@
	@undef=$$(nm -u $@); if [ -n "$$undef" ]; then \
	    echo "user.o must be self-contained, but needs: $$undef"; rm -f $@; exit 1; fi

# ---- images ----

$(BUILD)/kernel.elf: $(ARCH_OBJS) $(BUILD)/kernel.o $(BUILD)/user.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ARCH_OBJS) $(BUILD)/kernel.o $(BUILD)/user.o

# QEMU's multiboot loader takes only 32-bit ELF files; the code inside is
# the same 64-bit kernel.
$(BUILD)/%32.elf: $(BUILD)/%.elf
	$(OBJCOPY) -O elf32-i386 $< $@

QEMU_FLAGS := -m 128M -serial stdio -display none -no-reboot

run: $(BUILD)/kernel32.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $<

test: $(BUILD)/kernel32.elf
	./tools/qemu_test.sh $< "[root] all " "checks passed"

# ---- the arch layer on its own ----

$(BUILD)/archtest/core.o: tools/archtest/core.c $(ARCH)/hal.h | $(BUILD)
	$(CC) $(CFLAGS_ARCH) -c $< -o $@

$(BUILD)/archtest/user.o: tools/archtest/user.c user/syscall.h | $(BUILD)
	$(CC) $(CFLAGS_USER) -c $< -o $@
	@undef=$$(nm -u $@); if [ -n "$$undef" ]; then \
	    echo "archtest user.o must be self-contained, but needs: $$undef"; rm -f $@; exit 1; fi

$(BUILD)/archtest.elf: $(ARCH_OBJS) $(BUILD)/archtest/core.o $(BUILD)/archtest/user.o $(ARCH)/linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ARCH_OBJS) $(BUILD)/archtest/core.o $(BUILD)/archtest/user.o

archtest: $(BUILD)/archtest32.elf
	./tools/qemu_test.sh $< "[archtest] PASS"

run-archtest: $(BUILD)/archtest32.elf
	$(QEMU) $(QEMU_FLAGS) -kernel $<

clean:
	rm -rf $(BUILD)
