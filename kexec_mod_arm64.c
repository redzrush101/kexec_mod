// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - ARM64 Architecture Support
 *
 * Implements ARM64-specific machine_kexec functionality including
 * cache flushing, CPU state preparation, and kernel transition.
 *
 * Based on arch/arm64/kernel/machine_kexec.c
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>

#include <asm/cacheflush.h>
#include <asm/daifflags.h>
#include <asm/memory.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/page.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

#include "kexec_mod.h"

/* ============================================================
 * External Assembly Symbols
 * ============================================================ */

/* 
 * Relocation code - defined in kexec_mod_relocate.S
 * This code handles MMU disable AND relocation.
 * 
 * Arguments:
 *   x0: el2_switch (unused, pass 0)
 *   x1: physical address of this code (control page phys)
 *   x2: kimage->head (relocation list)
 *   x3: kimage->start (kernel entry point)
 *   x4: dtb_mem (device tree, or 0)
 */
extern const unsigned char kexec_mod_relocate_new_kernel[];
extern const unsigned char kexec_mod_relocate_new_kernel_end[];

/* ============================================================
 * Debug Helpers
 * ============================================================ */

static void kexec_mod_image_info(const struct kexec_mod_image *image)
{
    unsigned long i;

    kexec_mod_info("kexec image info:\n");
    kexec_mod_info("  type:        %d\n", image->type);
    kexec_mod_info("  start:       %lx\n", image->start);
    kexec_mod_info("  head:        %lx\n", image->head);
    kexec_mod_info("  nr_segments: %lu\n", image->nr_segments);

    for (i = 0; i < image->nr_segments; i++) {
        kexec_mod_info("  segment[%lu]: %016lx - %016lx, %lu bytes\n",
                      i,
                      image->segment[i].mem,
                      image->segment[i].mem + image->segment[i].memsz,
                      (unsigned long)image->segment[i].memsz);
    }
}

/* ============================================================
 * Cache Flushing
 * ============================================================ */

static void kexec_mod_list_flush(struct kexec_mod_image *image)
{
    kexec_mod_entry_t *entry;

    for (entry = &image->head; ; entry++) {
        unsigned int flag;
        void *addr;

        kexec_sym___flush_dcache_area(entry, sizeof(kexec_mod_entry_t));

        flag = *entry & IND_FLAGS;
        if (flag == IND_DONE)
            break;

        addr = phys_to_virt(*entry & PAGE_MASK);

        switch (flag) {
        case IND_INDIRECTION:
            entry = (kexec_mod_entry_t *)addr - 1;
            break;
        case IND_SOURCE:
            kexec_sym___flush_dcache_area(addr, PAGE_SIZE);
            break;
        case IND_DESTINATION:
            break;
        default:
            break;
        }
    }
}

static void kexec_mod_segment_flush(const struct kexec_mod_image *image)
{
    unsigned long i;

    for (i = 0; i < image->nr_segments; i++) {
        void *addr = phys_to_virt(image->segment[i].mem);
        kexec_sym___flush_dcache_area(addr, image->segment[i].memsz);
    }
}

/* ============================================================
 * Machine Kexec Implementation
 * ============================================================ */

int kexec_mod_machine_prepare(struct kexec_mod_image *image)
{
    kexec_mod_image_info(image);
    return 0;
}

void kexec_mod_machine_cleanup(struct kexec_mod_image *image)
{
    /* Nothing to clean up */
}

/**
 * kexec_mod_machine_kexec - Execute the loaded kernel
 * @image: Image to execute
 *
 * This function does not return on success.
 */
void __attribute__((no_sanitize("cfi"))) kexec_mod_machine_kexec(struct kexec_mod_image *image)
{
    phys_addr_t code_phys;
    phys_addr_t page_table_phys;
    struct page *pt_page;
    struct page *code_page;
    void *code_virt;
    unsigned long code_size;
    
    void (*relocate_fn)(unsigned long, unsigned long, unsigned long,
                        unsigned long, unsigned long, unsigned long,
                        unsigned long);

    /* Calculate code size */
    /* Calculate code size */
    code_size = (unsigned long)kexec_mod_relocate_new_kernel_end - 
                (unsigned long)kexec_mod_relocate_new_kernel;
                
    kexec_mod_info("Starting kexec transition [Point A]\n");
    kexec_mod_info("Relocation code size: %lu bytes\n", code_size);
    kexec_mod_image_info(image);

    /* 
     * 1. Allocate Page Table Page (Identity Map)
     */
    kexec_mod_info("[Point B] Allocating page table\n");
    pt_page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!pt_page) {
        kexec_mod_err("Failed to allocate page table\n");
        return;
    }
    page_table_phys = page_to_phys(pt_page);
    
    /* 
     * 2. Allocate Code Page (Relocation Stub destination)
     */
    kexec_mod_info("[Point C] Allocating code page\n");
    code_page = alloc_pages(GFP_KERNEL, get_order(code_size));
    if (!code_page) {
        kexec_mod_err("Failed to allocate code page\n");
        __free_page(pt_page);
        return;
    }
    code_phys = page_to_phys(code_page);
    code_virt = page_address(code_page);
    
    kexec_mod_info("Page table: phys=0x%llx\n", page_table_phys);
    kexec_mod_info("Code page: virt=%px, phys=0x%llx\n", code_virt, code_phys);

    /* 3. Copy relocation code to the contiguous page */
    kexec_mod_info("[Point D] Copying relocation code to %px...\n", code_virt);
    memcpy(code_virt, kexec_mod_relocate_new_kernel, code_size);
    
    /* Flush caches for the code copy */
    kexec_mod_info("[Point E] Flushing dcache...\n");
    kexec_sym___flush_dcache_area(code_virt, code_size);
    
    kexec_mod_info("[Point F] Flushing icache...\n");
    kexec_mod_info("[Point F1] code_virt=%px, code_size=%lu\n", code_virt, code_size);
    
    /*
     * IMPORTANT: flush_icache_range may send IPIs to other CPUs.
     * This MUST be called BEFORE SMP shutdown or use local-only cache ops.
     * Since we're called after SMP shutdown, use local invalidation only.
     */
    kexec_mod_info("[Point F2] Using local cache invalidation (no IPI)...\n");
    
    /* Use __flush_dcache_area which is local-only */
    kexec_sym___flush_dcache_area(code_virt, code_size);
    
    /* Synchronization barriers */
    dsb(sy);
    
    /* Invalidate I-cache locally - this should work after SMP stop */
    asm volatile("ic iallu\n" "dsb sy\n" "isb\n" ::: "memory");
    
    kexec_mod_info("[Point F3] Local cache flush done\n");
    
    kexec_mod_info("[Point G] Cache flush complete.\n");

    kexec_mod_image_info(image);

    /* Flush the relocation list and source pages */
    kexec_mod_info("[Point H] Flushing list...\n");
    kexec_mod_list_flush(image);
    if (image->head & IND_DONE) {
        kexec_mod_info("[Point I] Flushing segments...\n");
        kexec_mod_segment_flush(image);
    }

    /* Disable interrupts */
    kexec_mod_info("[Point J] Masking interrupts...\n");
    local_daif_mask();

    kexec_mod_info("[Point K] Bye! Jumping to relocation code...\n");

    /*
     * Call the LOCAL relocation function (in module text).
     * It will:
     * 1. Setup identity map pointing to 'code_phys'
     * 2. Jump to 'code_phys'
     */
    relocate_fn = (void (*)(unsigned long, unsigned long, unsigned long,
                            unsigned long, unsigned long, unsigned long,
                            unsigned long))
                  kexec_mod_relocate_new_kernel;
    
    kexec_mod_info("[Point L] Calling relocate_fn at %px...\n", relocate_fn);
    relocate_fn(0,                  /* el2_switch = 0 */
                code_phys,          /* physical addr of code COPY */
                image->head,        /* relocation list */
                image->start,       /* kernel entry point */
                image->dtb_mem,     /* dtb address */
                page_table_phys,    /* page table physical address */
                (unsigned long)page_address(pt_page)); /* page table virtual address */

    /* Should never reach here */
    kexec_mod_err("[Point M] Relocation returned unexpectedly!\n");
    unreachable();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kexec Module Author");
MODULE_DESCRIPTION("Standalone Kexec ARM64 Support");
