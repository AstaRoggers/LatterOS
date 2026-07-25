# Nuke built-in rules.
.SUFFIXES:

# Final kernel executable name.
override OUTPUT := kernel

# Target architecture.
ARCH := x86_64

# Install prefix.
PREFIX := /usr/local

# Required interrupt assembly source.
override INTERRUPT_STUB_FILE := src/interrupt_stubs.S

# Check if the architecture is supported.
ifeq ($(filter $(ARCH),aarch64 loongarch64 riscv64 x86_64),)
    $(error Architecture $(ARCH) not supported)
endif

# User-controllable toolchain.
TOOLCHAIN :=
TOOLCHAIN_PREFIX :=

ifneq ($(TOOLCHAIN),)
    ifeq ($(TOOLCHAIN_PREFIX),)
        TOOLCHAIN_PREFIX := $(TOOLCHAIN)-
    endif
endif

# C compiler.
ifneq ($(TOOLCHAIN_PREFIX),)
    CC := $(TOOLCHAIN_PREFIX)gcc
else
    CC := cc
endif

# Linker.
LD := $(TOOLCHAIN_PREFIX)ld

# LLVM toolchain configuration.
ifeq ($(TOOLCHAIN),llvm)
    CC := clang
    LD := ld.lld
endif

# User-controllable compiler flags.
CFLAGS := -g -O2 -pipe

# User-controllable preprocessor flags.
CPPFLAGS :=

ifeq ($(ARCH),x86_64)
    NASMFLAGS := -g
endif

# User-controllable linker flags.
LDFLAGS :=

# Ensure dependencies have been obtained.
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),default)),)
    ifeq ($(wildcard .deps-obtained),)
        $(error Please run the ./get-deps script first)
    endif
endif

# Ensure the interrupt assembly file exists.
ifneq ($(filter-out clean distclean,$(or $(MAKECMDGOALS),default)),)
    ifeq ($(wildcard $(INTERRUPT_STUB_FILE)),)
        $(error Missing $(INTERRUPT_STUB_FILE). Put interrupt_stubs.S inside kernel/src)
    endif
endif

# Check whether CC is Clang.
override CC_IS_CLANG := $(shell ! $(CC) --version 2>/dev/null | grep -q '^Target: '; echo $$?)

# Internal C flags.
override CFLAGS += \
    -Wall \
    -Wextra \
    -std=gnu11 \
    -nostdinc \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIC \
    -ffunction-sections \
    -fdata-sections

# Internal preprocessor flags.
override CPPFLAGS := \
    -I src \
    -I limine-protocol/include \
    -isystem freestanding-c-hdrs/include \
    $(CPPFLAGS) \
    -MMD \
    -MP

ifeq ($(ARCH),x86_64)
    override NASMFLAGS := \
        $(patsubst -g,-g -F dwarf,$(NASMFLAGS)) \
        -Wall
endif

# Architecture-specific flags.
ifeq ($(ARCH),x86_64)

    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target x86_64-unknown-none-elf
    endif

    override CFLAGS += \
        -m64 \
        -march=x86-64 \
        -mabi=sysv \
        -mno-80387 \
        -mno-mmx \
        -mno-sse \
        -mno-sse2 \
        -mno-red-zone \
        -mcmodel=kernel

    override LDFLAGS += \
        -m elf_x86_64

    override NASMFLAGS := \
        -f elf64 \
        $(NASMFLAGS)

endif

ifeq ($(ARCH),aarch64)

    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target aarch64-unknown-none-elf
    endif

    override CFLAGS += \
        -mcpu=generic \
        -march=armv8-a+nofp+nosimd \
        -mgeneral-regs-only

    override LDFLAGS += \
        -m aarch64elf

endif

ifeq ($(ARCH),riscv64)

    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target riscv64-unknown-none-elf
    endif

    override CFLAGS += \
        -march=rv64imac_zicsr_zifencei \
        -mabi=lp64 \
        -mno-relax

    override LDFLAGS += \
        -m elf64lriscv \
        --no-relax

endif

ifeq ($(ARCH),loongarch64)

    ifeq ($(CC_IS_CLANG),1)
        override CC += \
            -target loongarch64-unknown-none-elf
    endif

    override CFLAGS += \
        -march=loongarch64 \
        -mabi=lp64s \
        -mfpu=none \
        -msimd=none

    override LDFLAGS += \
        -m elf64loongarch

endif

# Internal linker flags.
override LDFLAGS += \
    -nostdlib \
    -static \
    -z max-page-size=0x1000 \
    --gc-sections \
    -T linker-scripts/$(ARCH).lds

# Find source files.
override SRCFILES := $(shell find -L src cc-runtime/src \
    -type f \
    -not -path 'src/arch/*' \
    2>/dev/null | LC_ALL=C sort)

# Add architecture-specific files.
override SRCFILES += $(shell find -L src/arch/$(ARCH) \
    -type f \
    2>/dev/null | LC_ALL=C sort)

# Explicitly include interrupt_stubs.S.
override SRCFILES += $(INTERRUPT_STUB_FILE)

# Remove duplicate source paths.
override SRCFILES := $(sort $(SRCFILES))

# Separate source types.
override CFILES := $(filter %.c,$(SRCFILES))
override ASFILES := $(filter %.S,$(SRCFILES))

ifeq ($(ARCH),x86_64)
    override NASMFILES := $(filter %.asm,$(SRCFILES))
endif

# Generate object paths.
override OBJ := $(addprefix obj-$(ARCH)/,\
    $(CFILES:.c=.c.o) \
    $(ASFILES:.S=.S.o))

ifeq ($(ARCH),x86_64)
    override OBJ += $(addprefix obj-$(ARCH)/,\
        $(NASMFILES:.asm=.asm.o))
endif

# Header dependency files.
override HEADER_DEPS := $(addprefix obj-$(ARCH)/,\
    $(CFILES:.c=.c.d) \
    $(ASFILES:.S=.S.d))

# Default target.
.PHONY: all
all: bin-$(ARCH)/$(OUTPUT)

# Include generated dependencies.
-include $(HEADER_DEPS)

# Link the kernel.
bin-$(ARCH)/$(OUTPUT): GNUmakefile linker-scripts/$(ARCH).lds $(OBJ)
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(OBJ) -o $@

# Compile C files.
obj-$(ARCH)/%.c.o: %.c GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Compile preprocessed assembly files.
obj-$(ARCH)/%.S.o: %.S GNUmakefile
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

ifeq ($(ARCH),x86_64)

# Compile NASM files.
obj-$(ARCH)/%.asm.o: %.asm GNUmakefile
	mkdir -p "$(dir $@)"
	nasm $(NASMFLAGS) $< -o $@

endif

# Remove compiled files.
.PHONY: clean
clean:
	rm -rf bin-$(ARCH) obj-$(ARCH)

# Remove compiled files and dependencies.
.PHONY: distclean
distclean:
	rm -rf \
	    bin-* \
	    obj-* \
	    .deps-obtained \
	    .cache \
	    compile_commands.json \
	    freestanding-c-hdrs \
	    cc-runtime \
	    limine-protocol

# Install kernel.
.PHONY: install
install: all
	install -d "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"
	install -m 644 \
	    bin-$(ARCH)/$(OUTPUT) \
	    "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH)"

# Uninstall kernel.
.PHONY: uninstall
uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)/$(OUTPUT)-$(ARCH)"
	-rmdir "$(DESTDIR)$(PREFIX)/share/$(OUTPUT)"