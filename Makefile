# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for Standalone Kexec Module
#

obj-$(CONFIG_KEXEC_MOD) := kexec_mod.o

kexec_mod-y := kexec_mod_main.o kexec_mod_core.o kexec_mod_memory.o \
                kexec_mod_smp.o kexec_mod_arm64.o kexec_mod_relocate.o \
                kexec_mod_symbols.o kexec_mod_psci.o

ccflags-y := -DKEXEC_MOD_DEBUG=1
ccflags-y += -I$(src)
