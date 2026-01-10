// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - SMP Shutdown
 *
 * Implements CPU shutdown sequence and interrupt masking
 * for safe kexec transition.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/irqdesc.h>
#include <linux/reboot.h>
#include <linux/notifier.h>
#include <linux/stop_machine.h>

#include "kexec_mod.h"

/* ============================================================
 * CPU Stop via IPI
 * ============================================================ */

static atomic_t cpus_stopped = ATOMIC_INIT(0);

/* Function called on each secondary CPU to stop it */
static void kexec_mod_cpu_stop(void *unused)
{
    /* Disable interrupts on this CPU */
    local_irq_disable();
    
    /* Signal that we stopped */
    atomic_inc(&cpus_stopped);
    
    kexec_mod_dbg("CPU %d stopped\n", smp_processor_id());
    
    /* Try to power off via PSCI (does not return on success) */
    kexec_mod_psci_cpu_off();
    
    /* Spin forever if PSCI fails - we'll never return */
    while (1) {
        cpu_relax();
        wfe();  /* Wait for event - low power wait */
    }
}

/* ============================================================
 * CPU Shutdown
 * ============================================================ */

/**
 * kexec_mod_smp_shutdown - Offline all secondary CPUs
 *
 * Returns: 0 on success, negative error on failure
 */
int kexec_mod_smp_shutdown(void)
{
    int cpu;
    int current_cpu = smp_processor_id();
    int num_cpus = num_online_cpus();
    unsigned long timeout;

    kexec_mod_info("Shutting down secondary CPUs (%d online)\n", num_cpus);

    /* If we're the only CPU, nothing to do */
    if (num_cpus == 1) {
        kexec_mod_info("Already on single CPU\n");
        return 0;
    }

    /* First, migrate to CPU 0 if not already there */
    if (current_cpu != 0 && cpu_online(0)) {
        kexec_mod_dbg("Migrating from CPU %d to CPU 0\n", current_cpu);
        current->flags |= PF_NO_SETAFFINITY;
        set_cpus_allowed_ptr(current, cpumask_of(0));
    }

    /* Disable CPU hotplug to prevent races */
    cpu_hotplug_disable();

    /* Try method 1: CPU hotplug (remove_cpu) */
    kexec_mod_info("Method 1: Trying CPU hotplug\n");
    for_each_online_cpu(cpu) {
        if (cpu == smp_processor_id())
            continue;

        kexec_mod_dbg("Trying to offline CPU %d via hotplug\n", cpu);
#ifdef CONFIG_HOTPLUG_CPU
        if (remove_cpu(cpu) == 0) {
            kexec_mod_dbg("CPU %d offlined successfully\n", cpu);
        } else {
            kexec_mod_dbg("CPU %d hotplug failed, will try IPI\n", cpu);
        }
#endif
    }

    /* Check if we succeeded */
    if (num_online_cpus() == 1) {
        kexec_mod_info("All secondary CPUs offlined via hotplug\n");
        cpu_hotplug_enable();
        return 0;
    }

    /* Method 2: Send IPI to stop remaining CPUs */
    kexec_mod_info("Method 2: Stopping remaining %d CPUs via IPI\n", 
                   num_online_cpus() - 1);
    
    atomic_set(&cpus_stopped, 0);
    
    /* Send function call to all other CPUs */
    smp_call_function(kexec_mod_cpu_stop, NULL, 0);  /* 0 = don't wait */
    
    /* Wait for CPUs to stop */
    timeout = 1000;  /* 1 second */
    while (atomic_read(&cpus_stopped) < (num_online_cpus() - 1) && timeout--) {
        mdelay(1);
    }
    
    if (atomic_read(&cpus_stopped) >= (num_online_cpus() - 1)) {
        kexec_mod_info("All secondary CPUs stopped via IPI\n");
    } else {
        kexec_mod_warn("Some CPUs may not have stopped (stopped=%d, expected=%d)\n",
                       atomic_read(&cpus_stopped), num_online_cpus() - 1);
    }

    cpu_hotplug_enable();
    
    /* Even if some CPUs didn't stop, continue anyway
     * The new kernel will re-initialize them */
    return 0;
}

/* ============================================================
 * Interrupt Masking
 * ============================================================ */

/**
 * kexec_mod_mask_interrupts - Mask all interrupts
 */
void kexec_mod_mask_interrupts(void)
{
    unsigned int i;
    struct irq_desc *desc;

    kexec_mod_info("Masking all interrupts\n");

    for_each_irq_desc(i, desc) {
        struct irq_chip *chip;
        int ret;

        chip = irq_desc_get_chip(desc);
        if (!chip)
            continue;

        /* Try to remove active state */
        ret = irq_set_irqchip_state(i, IRQCHIP_STATE_ACTIVE, false);

        if (ret && irqd_irq_inprogress(&desc->irq_data) && chip->irq_eoi)
            chip->irq_eoi(&desc->irq_data);

        /* Mask the interrupt */
        if (chip->irq_mask)
            chip->irq_mask(&desc->irq_data);

        /* Disable if not already disabled */
        if (chip->irq_disable && !irqd_irq_disabled(&desc->irq_data))
            chip->irq_disable(&desc->irq_data);
    }

    kexec_mod_dbg("All interrupts masked\n");
}

MODULE_LICENSE("GPL");
