# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Standalone Kexec Module
#
# Build with:
#   make -C /path/to/kernel M=$(pwd) modules ARCH=arm64 CROSS_COMPILE=aarch64-linux-android-
#
# Or from kernel source directory:
#   make M=kexec_mod modules
#

# Module name
obj-m := kexec_mod.o

# Object files that make up the module
kexec_mod-objs := kexec_mod_main.o kexec_mod_core.o kexec_mod_memory.o \
                kexec_mod_smp.o kexec_mod_arm64.o kexec_mod_relocate.o \
                kexec_mod_symbols.o kexec_mod_psci.o \
	kexec_mod_relocate.o

# Compiler flags
ccflags-y := -DKEXEC_MOD_DEBUG=1
ccflags-y += -I$(src)

# Assembly flags
asflags-y := -D__ASSEMBLY__

# For building outside of kernel tree (standalone)
KDIR ?= /lib/modules/$(shell uname -r)/build

# Default target when building standalone
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# Clean target
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Install target
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean install
