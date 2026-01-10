/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Standalone Kexec Module - PSCI Helpers
 */

#include <linux/linkage.h>

/* PSCI 0.2 CPU_OFF function ID */
#define PSCI_0_2_FN_CPU_OFF		0x84000002

	.text
	.align 3

/*
 * void kexec_mod_psci_cpu_off(void)
 *
 * Attempts to power off the calling CPU using PSCI via HVC or SMC.
 * This function does not return if successful.
 */
SYM_FUNC_START(kexec_mod_psci_cpu_off)
	/* Load CPU_OFF function ID into x0 */
	ldr	x0, =PSCI_0_2_FN_CPU_OFF
	
	/* Try HVC first (Standard for KVM/QEMU guests) */
	hvc	#0
	
	/* If HVC returns, it failed or wasn't handled. Try SMC. */
	ldr	x0, =PSCI_0_2_FN_CPU_OFF
	smc	#0
	
	/* If we get here, both failed. Return to C code. */
	ret
SYM_FUNC_END(kexec_mod_psci_cpu_off)
