// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - Core Functions
 *
 * Implements the core kexec functionality: image allocation,
 * segment loading, and relocation list management.
 *
 * Based on kernel/kexec_core.c
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>

#include "kexec_mod.h"

/* ============================================================
 * Image Allocation and Deallocation
 * ============================================================ */

/**
 * kexec_mod_image_alloc - Allocate and initialize a kexec image structure
 *
 * Returns: Pointer to new image structure, or NULL on failure
 */
struct kexec_mod_image *kexec_mod_image_alloc(void)
{
    struct kexec_mod_image *image;

    image = kzalloc(sizeof(*image), GFP_KERNEL);
    if (!image) {
        kexec_mod_err("Failed to allocate image structure\n");
        return NULL;
    }

    image->head = 0;
    image->entry = &image->head;
    image->last_entry = &image->head;
    image->control_page = ~0UL;  /* Not applicable by default */
    image->type = KEXEC_MOD_TYPE_DEFAULT;

    INIT_LIST_HEAD(&image->control_pages);
    INIT_LIST_HEAD(&image->dest_pages);
    INIT_LIST_HEAD(&image->unusable_pages);

    kexec_mod_dbg("Allocated new image structure at %px\n", image);

    return image;
}

/**
 * kexec_mod_image_free - Free a kexec image and all associated resources
 * @image: Image to free
 */
void kexec_mod_image_free(struct kexec_mod_image *image)
{
    kexec_mod_entry_t *ptr, entry;
    kexec_mod_entry_t ind = 0;

    if (!image)
        return;

    kexec_mod_dbg("Freeing image at %px\n", image);

    /* Free extra pages */
    kexec_mod_free_page_list(&image->dest_pages);
    kexec_mod_free_page_list(&image->unusable_pages);

    /* Free entries in the relocation list */
    for (ptr = &image->head; ; ptr++) {
        entry = *ptr;
        if (!entry || (entry & IND_DONE))
            break;

        if (entry & IND_INDIRECTION) {
            /* Free previous indirection page */
            if (ind & IND_INDIRECTION) {
                struct page *page = pfn_to_page(ind >> PAGE_SHIFT);
                kexec_mod_free_pages(page);
            }
            ind = entry;
            ptr = (kexec_mod_entry_t *)phys_to_virt(entry & PAGE_MASK);
            ptr--;  /* Will be incremented by loop */
        } else if (entry & IND_SOURCE) {
            struct page *page = pfn_to_page((entry & PAGE_MASK) >> PAGE_SHIFT);
            kexec_mod_free_pages(page);
        }
    }

    /* Free final indirection page */
    if (ind & IND_INDIRECTION) {
        struct page *page = pfn_to_page((ind & PAGE_MASK) >> PAGE_SHIFT);
        kexec_mod_free_pages(page);
    }

    /* Free control pages */
    kexec_mod_free_page_list(&image->control_pages);

    /* Cleanup architecture-specific resources */
    kexec_mod_machine_cleanup(image);

    kfree(image);
}

/* ============================================================
 * Segment Validation
 * ============================================================ */

/**
 * kexec_mod_is_destination_range - Check if range overlaps any segment destination
 * @image: Image to check
 * @start: Start address
 * @end:   End address
 *
 * Returns: 1 if overlap exists, 0 otherwise
 */
int kexec_mod_is_destination_range(struct kexec_mod_image *image,
                                   unsigned long start, unsigned long end)
{
    unsigned long i;

    for (i = 0; i < image->nr_segments; i++) {
        unsigned long mstart, mend;

        mstart = image->segment[i].mem;
        mend = mstart + image->segment[i].memsz;

        if ((end > mstart) && (start < mend))
            return 1;
    }

    return 0;
}

/**
 * kexec_mod_sanity_check - Validate segment list
 * @image: Image to validate
 *
 * Returns: 0 on success, negative error code on failure
 */
int kexec_mod_sanity_check(struct kexec_mod_image *image)
{
    unsigned long i, nr_segments = image->nr_segments;
    unsigned long total_pages = 0;
    unsigned long nr_pages = totalram_pages();

    kexec_mod_dbg("Sanity checking %lu segments\n", nr_segments);

    /* Check destination addresses are valid and page-aligned */
    for (i = 0; i < nr_segments; i++) {
        unsigned long mstart, mend;

        mstart = image->segment[i].mem;
        mend = mstart + image->segment[i].memsz;

        if (mstart > mend) {
            kexec_mod_err("Segment %lu: start > end\n", i);
            return -EADDRNOTAVAIL;
        }

        if ((mstart & ~PAGE_MASK) || (mend & ~PAGE_MASK)) {
            kexec_mod_err("Segment %lu: not page aligned\n", i);
            return -EADDRNOTAVAIL;
        }

        if (mend >= KEXEC_MOD_DESTINATION_MEMORY_LIMIT) {
            kexec_mod_err("Segment %lu: exceeds memory limit\n", i);
            return -EADDRNOTAVAIL;
        }
    }

    /* Check for overlapping segments */
    for (i = 0; i < nr_segments; i++) {
        unsigned long mstart, mend;
        unsigned long j;

        mstart = image->segment[i].mem;
        mend = mstart + image->segment[i].memsz;

        for (j = 0; j < i; j++) {
            unsigned long pstart, pend;

            pstart = image->segment[j].mem;
            pend = pstart + image->segment[j].memsz;

            if ((mend > pstart) && (mstart < pend)) {
                kexec_mod_err("Segments %lu and %lu overlap\n", i, j);
                return -EINVAL;
            }
        }
    }

    /* Check buffer sizes don't exceed memory sizes */
    for (i = 0; i < nr_segments; i++) {
        if (image->segment[i].bufsz > image->segment[i].memsz) {
            kexec_mod_err("Segment %lu: bufsz > memsz\n", i);
            return -EINVAL;
        }
    }

    /* Check total memory usage doesn't exceed half of RAM */
    for (i = 0; i < nr_segments; i++) {
        unsigned long pages = (image->segment[i].memsz + PAGE_SIZE - 1) >> PAGE_SHIFT;

        if (pages > nr_pages / 2) {
            kexec_mod_err("Segment %lu: too large\n", i);
            return -EINVAL;
        }

        total_pages += pages;
    }

    if (total_pages > nr_pages / 2) {
        kexec_mod_err("Total segment size too large\n");
        return -EINVAL;
    }

    kexec_mod_dbg("Sanity check passed\n");
    return 0;
}

/* ============================================================
 * Relocation List Management
 * ============================================================ */

/**
 * kexec_mod_add_entry - Add an entry to the relocation list
 * @image: Image to add entry to
 * @entry: Entry value
 *
 * Returns: 0 on success, negative error on failure
 */
static int kexec_mod_add_entry(struct kexec_mod_image *image,
                               kexec_mod_entry_t entry)
{
    if (*image->entry != 0)
        image->entry++;

    if (image->entry == image->last_entry) {
        kexec_mod_entry_t *ind_page;
        struct page *page;

        /* Allocate new indirection page */
        page = kexec_mod_alloc_page(image, GFP_KERNEL, ~0UL);
        if (!page) {
            kexec_mod_err("Failed to allocate indirection page\n");
            return -ENOMEM;
        }

        ind_page = page_address(page);
        *image->entry = virt_to_phys(ind_page) | IND_INDIRECTION;
        image->entry = ind_page;
        image->last_entry = ind_page +
            ((PAGE_SIZE / sizeof(kexec_mod_entry_t)) - 1);
    }

    *image->entry = entry;
    image->entry++;
    *image->entry = 0;

    return 0;
}

/**
 * kexec_mod_set_destination - Set destination for following pages
 * @image: Image
 * @destination: Physical destination address
 */
static int kexec_mod_set_destination(struct kexec_mod_image *image,
                                     unsigned long destination)
{
    destination &= PAGE_MASK;
    return kexec_mod_add_entry(image, destination | IND_DESTINATION);
}

/**
 * kexec_mod_add_page - Add a source page to the relocation list
 * @image: Image
 * @page: Physical address of source page
 */
static int kexec_mod_add_page(struct kexec_mod_image *image, unsigned long page)
{
    page &= PAGE_MASK;
    return kexec_mod_add_entry(image, page | IND_SOURCE);
}

/**
 * kexec_mod_terminate - Mark the relocation list as complete
 * @image: Image
 */
void kexec_mod_terminate(struct kexec_mod_image *image)
{
    if (*image->entry != 0)
        image->entry++;

    *image->entry = IND_DONE;

    kexec_mod_dbg("Terminated relocation list\n");
}

/* ============================================================
 * Segment Loading
 * ============================================================ */

/**
 * kexec_mod_load_segment - Load a single segment from userspace
 * @image: Image to load segment into
 * @segment: Segment descriptor
 *
 * Returns: 0 on success, negative error on failure
 */
int kexec_mod_load_segment(struct kexec_mod_image *image,
                           struct kexec_mod_segment *segment)
{
    unsigned long maddr;
    size_t ubytes, mbytes;
    int result = 0;
    unsigned char __user *buf;

    buf = segment->buf;
    ubytes = segment->bufsz;
    mbytes = segment->memsz;
    maddr = segment->mem;

    kexec_mod_dbg("Loading segment: buf=%px, bufsz=%zu, mem=%lx, memsz=%zu\n",
                  buf, ubytes, maddr, mbytes);

    result = kexec_mod_set_destination(image, maddr);
    if (result < 0)
        return result;

    while (mbytes) {
        struct page *page;
        char *ptr;
        size_t uchunk, mchunk;

        page = kexec_mod_alloc_page(image, GFP_HIGHUSER, maddr);
        if (!page) {
            kexec_mod_err("Failed to allocate page for segment\n");
            return -ENOMEM;
        }

        result = kexec_mod_add_page(image, page_to_phys(page));
        if (result < 0)
            return result;

        ptr = kmap(page);

        /* Calculate chunk sizes */
        mchunk = min_t(size_t, mbytes, PAGE_SIZE);
        uchunk = min(ubytes, mchunk);

        /* Copy data from userspace */
        if (uchunk) {
            if (copy_from_user(ptr, buf, uchunk)) {
                kunmap(page);
                kexec_mod_err("Failed to copy from user\n");
                return -EFAULT;
            }
            buf += uchunk;
            ubytes -= uchunk;
        }

        /* Zero remaining bytes if memsz > bufsz */
        if (mchunk > uchunk)
            memset(ptr + uchunk, 0, mchunk - uchunk);

        kunmap(page);

        maddr += mchunk;
        mbytes -= mchunk;
    }

    kexec_mod_dbg("Segment loaded successfully\n");
    return 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kexec Module Author");
MODULE_DESCRIPTION("Standalone Kexec Core Functions");
