// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - Symbol Resolution and Fallbacks
 *
 * Provides fallback implementations when kernel symbols cannot be resolved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/cache.h>

#include "kexec_mod.h"

/* ============================================================
 * Fallback Cache Operations
 * 
 * When __flush_dcache_area is not available, we implement our own
 * using inline assembly.
 * ============================================================ */

static void kexec_mod_flush_dcache_fallback(void *addr, size_t len)
{
    unsigned long start = (unsigned long)addr;
    unsigned long end = start + len;
    unsigned long line_size;
    unsigned long ctr_el0;
    
    /* Get cache line size from CTR_EL0 */
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
    line_size = 4 << ((ctr_el0 >> 16) & 0xf);  /* DminLine field */
    
    /* Align start to cache line boundary */
    start &= ~(line_size - 1);
    
    /* Clean and invalidate each cache line */
    while (start < end) {
        asm volatile("dc civac, %0" : : "r"(start) : "memory");
        start += line_size;
    }
    
    /* Data synchronization barrier */
    asm volatile("dsb sy" : : : "memory");
}

/* ============================================================
 * Symbol Storage - with fallback function pointers
 * ============================================================ */

/* Function pointers - initialized to fallbacks or NULL */
void (*kexec_sym_device_shutdown)(void) = NULL;
void (*kexec_sym_syscore_shutdown)(void) = NULL;
void (*kexec_sym___flush_dcache_area)(void *addr, size_t len) = kexec_mod_flush_dcache_fallback;

/* Data pointers */
struct blocking_notifier_head *kexec_sym_reboot_notifier_list = NULL;

/* ============================================================
 * Symbol Resolution - Best Effort
 * ============================================================ */

/* ============================================================
 * Symbol Resolution - KPROBE lookup for kallsyms
 * ============================================================ */
#include <linux/kprobes.h>

static unsigned long lookup_name(const char *name)
{
    struct kprobe kp = {
        .symbol_name = name,
    };
    unsigned long addr;

    if (register_kprobe(&kp) < 0) {
        return 0;
    }
    addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);
    
    return addr;
}

int __attribute__((no_sanitize("cfi"))) kexec_mod_resolve_symbols(void)
{
    unsigned long (*kallsyms_lookup_name_ptr)(const char *name) = NULL;

    kexec_mod_info("Resolving symbols...\n");

    /* 1. Try to find kallsyms_lookup_name via kprobe */
    kallsyms_lookup_name_ptr = (void *)lookup_name("kallsyms_lookup_name");
    
    if (!kallsyms_lookup_name_ptr) {
        kexec_mod_err("Failed to lookup kallsyms_lookup_name. Cannot resolve symbols!\n");
        return -EFAULT;
    }
    kexec_mod_info("Found kallsyms_lookup_name at 0x%px\n", kallsyms_lookup_name_ptr);

    /* 2. Resolve required symbols */
    kexec_sym_device_shutdown = (void *)kallsyms_lookup_name_ptr("device_shutdown");
    if (kexec_sym_device_shutdown) 
        kexec_mod_info("  Bound device_shutdown\n");
    else 
        kexec_mod_info("  Failed to bind device_shutdown (RISKY)\n");

    kexec_sym_syscore_shutdown = (void *)kallsyms_lookup_name_ptr("syscore_shutdown");
    if (kexec_sym_syscore_shutdown)
        kexec_mod_info("  Bound syscore_shutdown\n");

    kexec_sym_reboot_notifier_list = (void *)kallsyms_lookup_name_ptr("reboot_notifier_list");
    if (kexec_sym_reboot_notifier_list)
        kexec_mod_info("  Bound reboot_notifier_list\n");

    /* Always use fallback for dcache flush to avoid CFI issues */
    kexec_mod_info("  Using fallback __flush_dcache_area (CFI safe)\n");

    return 0;
}

MODULE_LICENSE("GPL");
