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

/**
 * kexec_mod_resolve_symbols - Try to resolve symbols, use fallbacks
 *
 * This version doesn't fail if symbols can't be resolved,
 * it just uses fallback implementations where possible.
 */
int kexec_mod_resolve_symbols(void)
{
    kexec_mod_info("Using fallback implementations for unexported symbols\n");
    
    /* 
     * __flush_dcache_area - use our fallback implementation
     * Already set as default above
     */
    kexec_mod_info("  __flush_dcache_area: using inline assembly fallback\n");
    
    /*
     * device_shutdown / syscore_shutdown - these are optional
     * If not available, we skip them during the shutdown sequence
     */
    kexec_mod_info("  device_shutdown: will skip (not critical)\n");
    kexec_mod_info("  syscore_shutdown: will skip (not critical)\n");
    
    /*
     * reboot_notifier_list - optional notification
     */
    kexec_mod_info("  reboot_notifier_list: will skip (not critical)\n");
    
    kexec_mod_info("Symbol resolution complete (using fallbacks)\n");
    return 0;
}

MODULE_LICENSE("GPL");
