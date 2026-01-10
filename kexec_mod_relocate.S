/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone Kexec Module - ARM64 Relocation Code
 *
 * This code runs from a physically contiguous copy.
 * 1. Sets up identity mapping (while running virtually via module text alias)
 * 2. Jumps to physical address (via identity map)
 * 3. Disables MMU
 * 4. Relocates kernel
 * 5. Jumps to new kernel
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/page.h>

/* Entry list flags */
#define IND_DESTINATION_BIT     0
#define IND_INDIRECTION_BIT     1
#define IND_DONE_BIT            2
#define IND_SOURCE_BIT          3

/*
 * Arguments:
 *   x0: el2_switch (unused)  
 *   x1: physical address of this code COPY
 *   x2: kimage_head (physical address of relocation list)
 *   x3: kimage_start (entry point of new kernel, physical)  
 *   x4: dtb_mem (physical, or 0)
 *   x5: page table physical address
 *   x6: page table virtual address
 */
.text

SYM_CODE_START(kexec_mod_relocate_new_kernel)

    /* Save parameters */
    mov     x19, x0
    mov     x20, x1             /* x20 = code copy phys addr */
    mov     x21, x2
    mov     x22, x3
    mov     x23, x4
    mov     x24, x5             /* x24 = page table phys addr */
    mov     x26, x6             /* x26 = page table virt addr */

    /* Disable data cache */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 2)   /* Clear C bit */
    msr     sctlr_el1, x0
    isb
    dsb     sy

    /* Initialize page table using VIRTUAL address (x26) */
    mov     x0, x26
    mov     x1, x0
    mov     x2, #4096
    mov     x3, #0
1:  stp     x3, x3, [x1], #16
    subs    x2, x2, #16
    b.gt    1b
    dsb     sy

    /* Configure MAIR_EL1: Index 0 = Normal Memory (0xFF) */
    mov     x1, #0xFF
    msr     mair_el1, x1
    isb
    
    /* 
     * Block Descriptor Attributes (0x701):
     * AF=1 (bit 10), SH=OuterShareable(2) (bits 9:8=10), AP=RW(0)(bits 7:6=00).. wait AP
     * AP[1] = 0 (RW), AP[0] needs to be ?
     * AP=00 (RW EL1).
     * 0x701:
     * [11:10] = 01 (AF=0, ?) No.
     * AF is bit 10.
     * 0x401 = AF=1.
     * 0x701 -> 0111 0000 0001
     * [10] = 1 (AF)
     * [9:8] = 11 (SH=3 Inner Shareable)
     * [7:6] = 00 (AP=00 RW EL1)
     * [5] = 0
     * [4:2] = 000 (Idx 0)
     * [1:0] = 01 (Block)
     * So 0x701 = AF=1, SH=3(IS), AP=RW, Idx=0.
     */
    mov     x1, #0x00000000
    mov     x2, #0x701
    orr     x1, x1, x2
    str     x1, [x0, #0]        /* Index 0: 0-1GB */
    
    mov     x1, #0x40000000
    orr     x1, x1, x2
    str     x1, [x0, #8]        /* Index 1: 1GB-2GB */

    dsb     sy

    /*
     * Configure TCR_EL1 for our identity map (TTBR0).
     * We MUST preserve TTBR1 settings (bits 16+) because we are currently
     * executing in the TTBR1 range (module text). Modifying T1SZ or TG1
     * will cause an immediate translation fault on the next instruction fetch.
     *
     * Params for TTBR0:
     *   T0SZ = 25 (0x19) -> 39-bit VA space
     *   IRGN0 = 01 (WBWA)
     *   ORGN0 = 01 (WBWA)
     *   SH0 = 10 (Outer Shareable)
     *   TG0 = 00 (4KB)
     *   EPD0 = 0 (Enable walk)
     *
     * Value for lower 16 bits:
     *   [5:0]   T0SZ = 0x19
     *   [9:8]   IRGN0 = 0x1
     *   [11:10] ORGN0 = 0x1
     *   [13:12] SH0 = 0x2
     *   Result: 0x2519
     */
    mrs     x1, tcr_el1
    mov     x2, #0xFFFF
    bic     x1, x1, x2              /* Clear lower 16 bits */
    mov     x2, #0x2519             /* Set our T0 parameters */
    orr     x1, x1, x2
    msr     tcr_el1, x1
    isb

    /* Enable TTBR0 */
    msr     ttbr0_el1, x24
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* Jump to physical address */
    adr     x0, mmu_off
    adr     x1, kexec_mod_relocate_new_kernel
    sub     x0, x0, x1
    add     x0, x20, x0
    br      x0

mmu_off:
    /* Disable MMU */
    mrs     x0, sctlr_el1
    bic     x0, x0, #1
    bic     x0, x0, #(1 << 2)
    bic     x0, x0, #(1 << 12)
    msr     sctlr_el1, x0
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* Relocation Loop */
    mov     x16, x21
    mov     x17, x22
    mov     x18, x23
    mov     x13, xzr
    mov     x14, xzr

    tbnz    x16, #IND_DONE_BIT, .Ldone

.Lloop:
    and     x12, x16, #PAGE_MASK
.Ltest_source:
    tbz     x16, #IND_SOURCE_BIT, .Ltest_indirection
    mov     x0, x13
    mov     x1, x12
    add     x2, x0, #PAGE_SIZE
.Lcopy_loop:
    ldp     x3, x4, [x1], #16
    ldp     x5, x6, [x1], #16
    ldp     x7, x8, [x1], #16
    ldp     x9, x10, [x1], #16
    stp     x3, x4, [x0], #16
    stp     x5, x6, [x0], #16
    stp     x7, x8, [x0], #16
    stp     x9, x10, [x0], #16
    cmp     x0, x2
    b.lo    .Lcopy_loop
    add     x13, x13, #PAGE_SIZE
    b       .Lnext
.Ltest_indirection:
    tbz     x16, #IND_INDIRECTION_BIT, .Ltest_destination
    mov     x14, x12
    b       .Lnext
.Ltest_destination:
    tbz     x16, #IND_DESTINATION_BIT, .Lnext
    mov     x13, x12
.Lnext:
    ldr     x16, [x14], #8
    tbz     x16, #IND_DONE_BIT, .Lloop

.Ldone:
    ic      iallu
    dsb     sy
    isb
    mov     x0, x18
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x17

.Lhang:
    wfe
    b       .Lhang

SYM_CODE_END(kexec_mod_relocate_new_kernel)

.globl kexec_mod_relocate_new_kernel_end
kexec_mod_relocate_new_kernel_end:
