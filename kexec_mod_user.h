/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Standalone Kexec Module - Userspace API Header
 *
 * This header defines the interface for userspace programs
 * to interact with the kexec_mod kernel module.
 *
 * Usage:
 *   1. Open /dev/kexec_mod
 *   2. Prepare kernel image segments
 *   3. Call ioctl(fd, KEXEC_MOD_IOCTL_LOAD, &args)
 *   4. Call ioctl(fd, KEXEC_MOD_IOCTL_EXEC) to reboot
 */

#ifndef _UAPI_KEXEC_MOD_H
#define _UAPI_KEXEC_MOD_H

#include <linux/ioctl.h>
#include <linux/types.h>

#ifndef __user
#define __user
#endif

/* Maximum number of segments */
#define KEXEC_MOD_SEGMENT_MAX   16

/* Device path */
#define KEXEC_MOD_DEVICE        "/dev/kexec_mod"

/**
 * struct kexec_mod_segment - Describes a kernel segment
 * @buf:    Pointer to buffer containing segment data
 * @bufsz:  Size of data in buffer
 * @mem:    Physical memory address for segment
 * @memsz:  Size of memory region (>= bufsz, excess is zeroed)
 */
struct kexec_mod_segment {
    void *buf;
    __kernel_size_t bufsz;
    unsigned long mem;
    __kernel_size_t memsz;
};

/**
 * struct kexec_mod_load_args - Arguments for loading a kernel
 * @entry:       Physical entry point address of new kernel
 * @nr_segments: Number of segments in segments array
 * @segments:    Pointer to array of segment descriptors
 * @flags:       Load flags (reserved, set to 0)
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

/* Load a kernel image (requires CAP_SYS_BOOT) */
#define KEXEC_MOD_IOCTL_LOAD    _IOW(KEXEC_MOD_IOCTL_MAGIC, 1, struct kexec_mod_load_args)

/* Execute the loaded kernel (does not return on success) */
#define KEXEC_MOD_IOCTL_EXEC    _IO(KEXEC_MOD_IOCTL_MAGIC, 2)

/* Unload the current kernel image */
#define KEXEC_MOD_IOCTL_UNLOAD  _IO(KEXEC_MOD_IOCTL_MAGIC, 3)

/* Get status: returns 1 if image loaded, 0 otherwise */
#define KEXEC_MOD_IOCTL_STATUS  _IOR(KEXEC_MOD_IOCTL_MAGIC, 4, int)

#endif /* _UAPI_KEXEC_MOD_H */
