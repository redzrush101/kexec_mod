// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - Memory Management
 *
 * Implements page allocation for control pages, source pages,
 * and destination page management.
 *
 * Based on kernel/kexec_core.c memory functions
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/gfp.h>

#include "kexec_mod.h"

/* ============================================================
 * Basic Page Allocation
 * ============================================================ */

/**
 * kexec_mod_alloc_pages - Allocate pages for kexec use
 * @gfp_mask: GFP flags
 * @order: Page order
 *
 * Returns: Allocated page or NULL
 */
struct page *kexec_mod_alloc_pages(gfp_t gfp_mask, unsigned int order)
{
    struct page *pages;

    if (fatal_signal_pending(current))
        return NULL;

    pages = alloc_pages(gfp_mask & ~__GFP_ZERO, order);
    if (pages) {
        unsigned int count, i;

        pages->mapping = NULL;
        set_page_private(pages, order);
        count = 1 << order;

        for (i = 0; i < count; i++)
            SetPageReserved(pages + i);

        /* Zero if requested */
        if (gfp_mask & __GFP_ZERO) {
            for (i = 0; i < count; i++)
                clear_highpage(pages + i);
        }
    }

    return pages;
}

/**
 * kexec_mod_free_pages - Free pages allocated by kexec_mod_alloc_pages
 * @page: Page to free
 */
void kexec_mod_free_pages(struct page *page)
{
    unsigned int order, count, i;

    if (!page)
        return;

    order = page_private(page);
    count = 1 << order;

    for (i = 0; i < count; i++)
        ClearPageReserved(page + i);

    __free_pages(page, order);
}

/**
 * kexec_mod_free_page_list - Free a list of pages
 * @list: List head
 */
void kexec_mod_free_page_list(struct list_head *list)
{
    struct page *page, *next;

    list_for_each_entry_safe(page, next, list, lru) {
        list_del(&page->lru);
        kexec_mod_free_pages(page);
    }
}

/* ============================================================
 * Control Page Allocation
 * ============================================================ */

/**
 * kexec_mod_alloc_control_pages - Allocate control pages for kexec
 * @image: Image to allocate for
 * @order: Page order
 *
 * Control pages must be allocated in memory that will be identity-mapped
 * during the kexec transition. They must not conflict with destination
 * addresses.
 *
 * Returns: Allocated page or NULL
 */
struct page *kexec_mod_alloc_control_pages(struct kexec_mod_image *image,
                                            unsigned int order)
{
    struct list_head extra_pages;
    struct page *pages = NULL;
    unsigned int count;

    count = 1 << order;
    INIT_LIST_HEAD(&extra_pages);

    /* Loop until we find a page that doesn't conflict with destinations */
    do {
        unsigned long pfn, epfn, addr, eaddr;

        pages = kexec_mod_alloc_pages(KEXEC_MOD_CONTROL_MEMORY_GFP, order);
        if (!pages)
            break;

        pfn = page_to_pfn(pages);
        epfn = pfn + count;
        addr = pfn << PAGE_SHIFT;
        eaddr = epfn << PAGE_SHIFT;

        /* Check if page exceeds limits or conflicts with destinations */
        if ((epfn >= (KEXEC_MOD_CONTROL_MEMORY_LIMIT >> PAGE_SHIFT)) ||
            kexec_mod_is_destination_range(image, addr, eaddr)) {
            /* Save for later cleanup and try again */
            list_add(&pages->lru, &extra_pages);
            pages = NULL;
        }
    } while (!pages);

    if (pages) {
        /* Remember the allocated page */
        list_add(&pages->lru, &image->control_pages);
        kexec_mod_dbg("Allocated control page at pfn %lx\n", page_to_pfn(pages));
    }

    /* Free extra pages we allocated but couldn't use */
    kexec_mod_free_page_list(&extra_pages);

    return pages;
}

/* ============================================================
 * Destination-Aware Page Allocation
 * ============================================================ */

/**
 * kexec_mod_dst_used - Check if destination address has a source page assigned
 * @image: Image to check
 * @page: Physical page address
 *
 * Returns: Pointer to entry if found, NULL otherwise
 */
static kexec_mod_entry_t *kexec_mod_dst_used(struct kexec_mod_image *image,
                                             unsigned long page)
{
    kexec_mod_entry_t *ptr, entry;
    unsigned long destination = 0;

    for (ptr = &image->head; ; ptr++) {
        entry = *ptr;
        if (!entry || (entry & IND_DONE))
            break;

        if (entry & IND_DESTINATION) {
            destination = entry & PAGE_MASK;
        } else if (entry & IND_SOURCE) {
            if (page == destination)
                return ptr;
            destination += PAGE_SIZE;
        } else if (entry & IND_INDIRECTION) {
            ptr = (kexec_mod_entry_t *)phys_to_virt(entry & PAGE_MASK);
            ptr--;  /* Will be incremented by loop */
        }
    }

    return NULL;
}

/**
 * kexec_mod_alloc_page - Allocate a page while respecting destinations
 * @image: Image to allocate for
 * @gfp_mask: GFP flags
 * @dest: Desired destination address (or ~0UL for any)
 *
 * This function ensures that source pages don't conflict with destination
 * pages that haven't been copied yet. It implements a safe allocation
 * strategy for the relocation process.
 *
 * Returns: Allocated page or NULL
 */
struct page *kexec_mod_alloc_page(struct kexec_mod_image *image,
                                  gfp_t gfp_mask, unsigned long dest)
{
    struct page *page;
    unsigned long addr;

    /* Check if we already have this destination page cached */
    list_for_each_entry(page, &image->dest_pages, lru) {
        addr = page_to_pfn(page) << PAGE_SHIFT;
        if (addr == dest) {
            list_del(&page->lru);
            return page;
        }
    }

    page = NULL;
    while (1) {
        kexec_mod_entry_t *old;

        /* Allocate a new page */
        page = kexec_mod_alloc_pages(gfp_mask, 0);
        if (!page)
            return NULL;

        /* Check if page exceeds source memory limit */
        if (page_to_pfn(page) > (KEXEC_MOD_SOURCE_MEMORY_LIMIT >> PAGE_SHIFT)) {
            list_add(&page->lru, &image->unusable_pages);
            continue;
        }

        addr = page_to_pfn(page) << PAGE_SHIFT;

        /* If this is exactly the destination we want, use it */
        if (addr == dest)
            break;

        /* If not a destination page at all, use it */
        if (!kexec_mod_is_destination_range(image, addr, addr + PAGE_SIZE))
            break;

        /* Page is someone's destination - check if it has a source */
        old = kexec_mod_dst_used(image, addr);
        if (old) {
            /* Swap source pages */
            unsigned long old_addr;
            struct page *old_page;

            old_addr = *old & PAGE_MASK;
            old_page = pfn_to_page(old_addr >> PAGE_SHIFT);
            copy_highpage(page, old_page);
            *old = addr | (*old & ~PAGE_MASK);

            /* Check if old page is usable with current gfp_mask */
            if (!(gfp_mask & __GFP_HIGHMEM) && PageHighMem(old_page)) {
                kexec_mod_free_pages(old_page);
                continue;
            }

            addr = old_addr;
            page = old_page;
            break;
        }

        /* Cache this destination page for later use */
        list_add(&page->lru, &image->dest_pages);
    }

    return page;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kexec Module Author");
MODULE_DESCRIPTION("Standalone Kexec Memory Management");
