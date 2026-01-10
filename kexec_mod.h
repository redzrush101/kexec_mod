/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone Kexec Module for Android Kernel 5.10
 * Core header file with data structures and definitions
 *
 * This module implements kexec functionality without requiring
 * CONFIG_KEXEC_CORE to be enabled in the kernel.
 */

#ifndef _KEXEC_MOD_H
#define _KEXEC_MOD_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/mm_types.h>

/* ============================================================
 * Constants and Limits
 * ============================================================ */

#define KEXEC_MOD_NAME          "kexec_mod"
#define KEXEC_MOD_VERSION       "1.0.0"

/* Maximum number of segments that can be loaded */
#define KEXEC_MOD_SEGMENT_MAX   16

/* Entry list flags (same as kernel kexec) */
#define IND_DESTINATION_BIT     0
#define IND_INDIRECTION_BIT     1
#define IND_DONE_BIT            2
#define IND_SOURCE_BIT          3

#define IND_DESTINATION         (1UL << IND_DESTINATION_BIT)
#define IND_INDIRECTION         (1UL << IND_INDIRECTION_BIT)
#define IND_DONE                (1UL << IND_DONE_BIT)
#define IND_SOURCE              (1UL << IND_SOURCE_BIT)
#define IND_FLAGS               (IND_DESTINATION | IND_INDIRECTION | IND_DONE | IND_SOURCE)

/* ARM64-specific limits */
#define KEXEC_MOD_SOURCE_MEMORY_LIMIT       (-1UL)
#define KEXEC_MOD_DESTINATION_MEMORY_LIMIT  (-1UL)
#define KEXEC_MOD_CONTROL_MEMORY_LIMIT      (-1UL)
#define KEXEC_MOD_CONTROL_PAGE_SIZE         4096

/* GFP flags for control page allocation */
#define KEXEC_MOD_CONTROL_MEMORY_GFP        (GFP_KERNEL | __GFP_NORETRY)

/* Image types */
#define KEXEC_MOD_TYPE_DEFAULT  0
#define KEXEC_MOD_TYPE_CRASH    1

/* ============================================================
 * Data Structures
 * ============================================================ */

/* Entry type for relocation list */
typedef unsigned long kexec_mod_entry_t;

/**
 * struct kexec_mod_segment - Describes a kernel segment to load
 * @buf:    Userspace buffer pointer
 * @bufsz:  Size of data in buffer
 * @mem:    Physical memory address where segment will be loaded
 * @memsz:  Size of memory region (may be larger than bufsz)
 */
struct kexec_mod_segment {
    void __user *buf;
    size_t bufsz;
    unsigned long mem;
    size_t memsz;
};

/**
 * struct kexec_mod_image - Complete kernel image ready for execution
 * @head:           First entry in relocation list
 * @entry:          Current entry pointer (for building list)
 * @last_entry:     Last available entry in current page
 * @start:          Entry point of new kernel
 * @control_code_page: Page containing relocation code
 * @swap_page:      Scratch page for relocation
 * @nr_segments:    Number of segments
 * @segment:        Array of segment descriptors
 * @control_pages:  List of allocated control pages
 * @dest_pages:     List of destination pages (pre-allocated)
 * @unusable_pages: List of pages that can't be used
 * @control_page:   Next control page address (for crash kernels)
 * @type:           Image type (DEFAULT or CRASH)
 */
struct kexec_mod_image {
    kexec_mod_entry_t head;
    kexec_mod_entry_t *entry;
    kexec_mod_entry_t *last_entry;

    unsigned long start;
    struct page *control_code_page;
    struct page *swap_page;

    unsigned long nr_segments;
    struct kexec_mod_segment segment[KEXEC_MOD_SEGMENT_MAX];

    struct list_head control_pages;
    struct list_head dest_pages;
    struct list_head unusable_pages;

    unsigned long control_page;
    unsigned int type;
    unsigned long dtb_mem;  /* Physical address of DTB */
};

/* ============================================================
 * IOCTL Interface Definitions
 * ============================================================ */

#define KEXEC_MOD_MAGIC         0x4B455845  /* "KEXE" */

/**
 * struct kexec_mod_load_args - Arguments for KEXEC_MOD_IOCTL_LOAD
 * @entry:          Entry point of new kernel
 * @nr_segments:    Number of segments to load
 * @segments:       Pointer to array of segment descriptors
 * @flags:          Load flags (reserved)
 */
struct kexec_mod_load_args {
    unsigned long entry;
    unsigned long nr_segments;
    struct kexec_mod_segment __user *segments;
    unsigned long flags;
    unsigned long dtb_mem;  /* Physical address of DTB */
};

/* IOCTL commands */
#define KEXEC_MOD_IOCTL_MAGIC   'K'
#define KEXEC_MOD_IOCTL_LOAD    _IOW(KEXEC_MOD_IOCTL_MAGIC, 1, struct kexec_mod_load_args)
#define KEXEC_MOD_IOCTL_EXEC    _IO(KEXEC_MOD_IOCTL_MAGIC, 2)
#define KEXEC_MOD_IOCTL_UNLOAD  _IO(KEXEC_MOD_IOCTL_MAGIC, 3)
#define KEXEC_MOD_IOCTL_STATUS  _IOR(KEXEC_MOD_IOCTL_MAGIC, 4, int)

/* ============================================================
 * Function Declarations - Core
 * ============================================================ */

/* kexec_mod_core.c */
struct kexec_mod_image *kexec_mod_image_alloc(void);
void kexec_mod_image_free(struct kexec_mod_image *image);
int kexec_mod_sanity_check(struct kexec_mod_image *image);
int kexec_mod_load_segment(struct kexec_mod_image *image,
                           struct kexec_mod_segment *segment);
void kexec_mod_terminate(struct kexec_mod_image *image);
int kexec_mod_is_destination_range(struct kexec_mod_image *image,
                                   unsigned long start, unsigned long end);

/* ============================================================
 * Function Declarations - Memory
 * ============================================================ */

/* kexec_mod_memory.c */
struct page *kexec_mod_alloc_pages(gfp_t gfp_mask, unsigned int order);
void kexec_mod_free_pages(struct page *page);
void kexec_mod_free_page_list(struct list_head *list);
struct page *kexec_mod_alloc_control_pages(struct kexec_mod_image *image,
                                           unsigned int order);
struct page *kexec_mod_alloc_page(struct kexec_mod_image *image,
                                  gfp_t gfp_mask, unsigned long dest);

/* ============================================================
 * Function Declarations - ARM64 Architecture
 * ============================================================ */

/* kexec_mod_arm64.c */
int kexec_mod_machine_prepare(struct kexec_mod_image *image);
void kexec_mod_machine_cleanup(struct kexec_mod_image *image);
void kexec_mod_machine_kexec(struct kexec_mod_image *image);

/* kexec_mod_relocate.S */
extern const unsigned char kexec_mod_relocate_new_kernel[];
extern const unsigned long kexec_mod_relocate_new_kernel_size;

/* ============================================================
 * Function Declarations - SMP
 * ============================================================ */

/* kexec_mod_smp.c */
int kexec_mod_smp_shutdown(void);
void kexec_mod_mask_interrupts(void);

/* kexec_mod_psci.S */
void kexec_mod_psci_cpu_off(void);

/* ============================================================
 * Resolved Symbol Pointers (kallsyms)
 * ============================================================ */

/* kexec_mod_symbols.c - must call kexec_mod_resolve_symbols() at init */
int kexec_mod_resolve_symbols(void);

/* Function pointers resolved at runtime */
extern void (*kexec_sym_device_shutdown)(void);
extern void (*kexec_sym_syscore_shutdown)(void);
extern void (*kexec_sym___flush_dcache_area)(void *addr, size_t len);
extern struct blocking_notifier_head *kexec_sym_reboot_notifier_list;

/* ============================================================
 * Global State
 * ============================================================ */

extern struct kexec_mod_image *kexec_mod_image;
extern struct mutex kexec_mod_mutex;

/* ============================================================
 * Debug Macros
 * ============================================================ */

#define KEXEC_MOD_DEBUG 1

#if KEXEC_MOD_DEBUG
#define kexec_mod_dbg(fmt, ...) \
    pr_debug(KEXEC_MOD_NAME ": " fmt, ##__VA_ARGS__)
#define kexec_mod_info(fmt, ...) \
    pr_info(KEXEC_MOD_NAME ": " fmt, ##__VA_ARGS__)
#else
#define kexec_mod_dbg(fmt, ...) do {} while(0)
#define kexec_mod_info(fmt, ...) do {} while(0)
#endif

#define kexec_mod_err(fmt, ...) \
    pr_err(KEXEC_MOD_NAME ": " fmt, ##__VA_ARGS__)

#define kexec_mod_warn(fmt, ...) \
    pr_warn(KEXEC_MOD_NAME ": " fmt, ##__VA_ARGS__)

#endif /* _KEXEC_MOD_H */
