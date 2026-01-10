/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone Kexec Module - CPU Reset Code
 *
 * Disables MMU and transitions CPU to physical addressing mode
 * before jumping to the relocation code.
 *
 * Based on arch/arm64/kernel/cpu-reset.S
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

/*
 * Note: For kernel modules, we can't use .idmap.text since modules
 * cannot have sections with both write and execute permissions.
 * The code will be copied to a control page before execution anyway.
 */
.text

/*
 * kexec_mod_cpu_soft_restart - Disable MMU and jump to relocation code
 *
 * This function:
 * 1. Disables the MMU
 * 2. Optionally switches to EL2 if needed
 * 3. Jumps to the relocation code with proper arguments
 *
 * Arguments:
 *   x0: el2_switch - Flag indicating if EL2 switch is needed
 *   x1: entry - Physical address of relocation code (reboot_code_buffer)
 *   x2: arg0 - First argument to relocation code (kimage_head)
 *   x3: arg1 - Second argument (kimage_start)
 *   x4: arg2 - Third argument (dtb_mem)
 *
 * This function does not return.
 */
SYM_CODE_START(kexec_mod_cpu_soft_restart)

    /* 
     * Disable MMU
     * Use mov_q to load the SCTLR value for MMU off
     * INIT_SCTLR_EL1_MMU_OFF = (3 << 28) | (3 << 22) | (1 << 20) | (1 << 11)
     *                       = 0x30D00800
     */
    mov_q   x12, INIT_SCTLR_EL1_MMU_OFF

    /* 
     * Workaround for Cortex-A53 erratum 819472
     * pre_disable_mmu_workaround is a macro that handles this
     */
    pre_disable_mmu_workaround

    /* Disable MMU at EL1 */
    msr     sctlr_el1, x12
    isb                             /* Ensure MMU is off */

    /* Check if we need to switch to EL2 */
    cbz     x0, 1f                  /* Skip if el2_switch == 0 */

    /*
     * Request EL2 to do the soft restart
     * HVC_SOFT_RESTART is defined in asm/virt.h
     */
    mov     x0, #HVC_SOFT_RESTART
    hvc     #0

    /* If HVC returns, fall through (shouldn't happen) */

1:
    /*
     * Set up arguments for relocation code and jump
     * 
     * Relocation code expects:
     *   x0 = kimage_head (relocation list)
     *   x1 = kimage_start (kernel entry point)
     *   x2 = dtb_mem (device tree address)
     */
    mov     x8, x1                  /* Save entry point */
    mov     x0, x2                  /* arg0 -> x0 (kimage_head) */
    mov     x1, x3                  /* arg1 -> x1 (kimage_start) */
    mov     x2, x4                  /* arg2 -> x2 (dtb_mem) */

    /* Jump to relocation code */
    br      x8

SYM_CODE_END(kexec_mod_cpu_soft_restart)
