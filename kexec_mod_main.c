// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone Kexec Module - Main Entry Point
 *
 * Module initialization, character device registration,
 * and IOCTL handler implementation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/uaccess.h>
#include <linux/slab.h>

#include "kexec_mod.h"

/* ============================================================
 * Global State
 * ============================================================ */

/* Currently loaded kexec image */
struct kexec_mod_image *kexec_mod_image = NULL;
EXPORT_SYMBOL(kexec_mod_image);

/* Mutex protecting kexec operations */
DEFINE_MUTEX(kexec_mod_mutex);
EXPORT_SYMBOL(kexec_mod_mutex);

/* Device structures */
static dev_t kexec_mod_dev;
static struct cdev kexec_mod_cdev;
static struct class *kexec_mod_class;

/* ============================================================
 * IOCTL Handlers
 * ============================================================ */

/**
 * kexec_mod_ioctl_load - Handle KEXEC_MOD_IOCTL_LOAD
 * @args: Load arguments from userspace
 *
 * Returns: 0 on success, negative error on failure
 */
static long kexec_mod_ioctl_load(struct kexec_mod_load_args __user *uargs)
{
    struct kexec_mod_load_args args;
    struct kexec_mod_segment *segments = NULL;
    struct kexec_mod_image *image = NULL;
    unsigned long i;
    int ret = 0;

    /* Copy arguments from userspace */
    if (copy_from_user(&args, uargs, sizeof(args))) {
        kexec_mod_err("Failed to copy load args from user\n");
        return -EFAULT;
    }

    /* Validate segment count */
    if (args.nr_segments > KEXEC_MOD_SEGMENT_MAX) {
        kexec_mod_err("Too many segments: %lu > %d\n",
                      args.nr_segments, KEXEC_MOD_SEGMENT_MAX);
        return -EINVAL;
    }

    /* If nr_segments is 0, unload current image */
    if (args.nr_segments == 0) {
        kexec_mod_info("Unloading current image\n");
        if (kexec_mod_image) {
            kexec_mod_image_free(kexec_mod_image);
            kexec_mod_image = NULL;
        }
        return 0;
    }

    /* Allocate and copy segments from userspace */
    segments = kmalloc_array(args.nr_segments, sizeof(*segments), GFP_KERNEL);
    if (!segments) {
        kexec_mod_err("Failed to allocate segments array\n");
        return -ENOMEM;
    }

    if (copy_from_user(segments, args.segments,
                       args.nr_segments * sizeof(*segments))) {
        kexec_mod_err("Failed to copy segments from user\n");
        ret = -EFAULT;
        goto out_free_segments;
    }

    /* Allocate new image */
    image = kexec_mod_image_alloc();
    if (!image) {
        ret = -ENOMEM;
        goto out_free_segments;
    }

    image->start = args.entry;
    image->nr_segments = args.nr_segments;
    image->dtb_mem = args.dtb_mem;
    memcpy(image->segment, segments, args.nr_segments * sizeof(*segments));

    kexec_mod_info("Loading kernel image: entry=%lx, segments=%lu, dtb=%lx\n",
                   args.entry, args.nr_segments, args.dtb_mem);

    /* Sanity check segments */
    ret = kexec_mod_sanity_check(image);
    if (ret)
        goto out_free_image;

    /* Allocate control code page */
    image->control_code_page = kexec_mod_alloc_control_pages(image,
        get_order(KEXEC_MOD_CONTROL_PAGE_SIZE));
    if (!image->control_code_page) {
        kexec_mod_err("Failed to allocate control code page\n");
        ret = -ENOMEM;
        goto out_free_image;
    }

    /* Allocate swap page for normal (non-crash) kexec */
    image->swap_page = kexec_mod_alloc_control_pages(image, 0);
    if (!image->swap_page) {
        kexec_mod_err("Failed to allocate swap page\n");
        ret = -ENOMEM;
        goto out_free_image;
    }

    /* Prepare architecture-specific resources */
    ret = kexec_mod_machine_prepare(image);
    if (ret)
        goto out_free_image;

    /* Load each segment */
    for (i = 0; i < args.nr_segments; i++) {
        ret = kexec_mod_load_segment(image, &image->segment[i]);
        if (ret) {
            kexec_mod_err("Failed to load segment %lu\n", i);
            goto out_free_image;
        }
    }

    /* Terminate the relocation list */
    kexec_mod_terminate(image);

    /* Free old image and install new one */
    if (kexec_mod_image)
        kexec_mod_image_free(kexec_mod_image);
    kexec_mod_image = image;
    image = NULL;

    kexec_mod_info("Kernel image loaded successfully\n");

out_free_image:
    if (image)
        kexec_mod_image_free(image);
out_free_segments:
    kfree(segments);
    return ret;
}

/**
 * kexec_mod_ioctl_exec - Handle KEXEC_MOD_IOCTL_EXEC
 *
 * Returns: Does not return on success
 */
static long kexec_mod_ioctl_exec(void)
{
    int ret;

    if (!kexec_mod_image) {
        kexec_mod_err("No image loaded\n");
        return -EINVAL;
    }

    kexec_mod_info("Executing kexec...\n");

    /* Shutdown secondary CPUs */
    ret = kexec_mod_smp_shutdown();
    if (ret) {
        kexec_mod_err("Failed to shutdown secondary CPUs\n");
        return ret;
    }

    /* Mask all interrupts */
    kexec_mod_mask_interrupts();

    /* Point of no return - execute the new kernel */
    kexec_mod_machine_kexec(kexec_mod_image);

    /* Should never reach here */
    kexec_mod_err("kexec returned unexpectedly!\n");
    return -EFAULT;
}

/**
 * kexec_mod_ioctl_status - Get current status
 * @status: Pointer to store status
 */
static long kexec_mod_ioctl_status(int __user *ustatus)
{
    int status = (kexec_mod_image != NULL) ? 1 : 0;

    if (put_user(status, ustatus))
        return -EFAULT;

    return 0;
}

/* ============================================================
 * File Operations
 * ============================================================ */

static int kexec_mod_open(struct inode *inode, struct file *file)
{
    kexec_mod_dbg("Device opened\n");
    return 0;
}

static int kexec_mod_release(struct inode *inode, struct file *file)
{
    kexec_mod_dbg("Device closed\n");
    return 0;
}

static long kexec_mod_ioctl(struct file *file, unsigned int cmd,
                            unsigned long arg)
{
    long ret;

    /* Check for CAP_SYS_BOOT capability */
    if (!capable(CAP_SYS_BOOT)) {
        kexec_mod_err("Permission denied: CAP_SYS_BOOT required\n");
        return -EPERM;
    }

    /* Take the mutex */
    if (!mutex_trylock(&kexec_mod_mutex)) {
        kexec_mod_err("Kexec operation in progress\n");
        return -EBUSY;
    }

    switch (cmd) {
    case KEXEC_MOD_IOCTL_LOAD:
        ret = kexec_mod_ioctl_load((struct kexec_mod_load_args __user *)arg);
        break;

    case KEXEC_MOD_IOCTL_EXEC:
        ret = kexec_mod_ioctl_exec();
        break;

    case KEXEC_MOD_IOCTL_UNLOAD:
        kexec_mod_info("Unloading image\n");
        if (kexec_mod_image) {
            kexec_mod_image_free(kexec_mod_image);
            kexec_mod_image = NULL;
        }
        ret = 0;
        break;

    case KEXEC_MOD_IOCTL_STATUS:
        ret = kexec_mod_ioctl_status((int __user *)arg);
        break;

    default:
        kexec_mod_err("Unknown ioctl command: 0x%x\n", cmd);
        ret = -EINVAL;
        break;
    }

    mutex_unlock(&kexec_mod_mutex);
    return ret;
}

static const struct file_operations kexec_mod_fops = {
    .owner          = THIS_MODULE,
    .open           = kexec_mod_open,
    .release        = kexec_mod_release,
    .unlocked_ioctl = kexec_mod_ioctl,
    .compat_ioctl   = kexec_mod_ioctl,
};

/* ============================================================
 * Module Init/Exit
 * ============================================================ */

static int __init kexec_mod_init(void)
{
    int ret;
    struct device *dev;

    kexec_mod_info("Initializing %s v%s\n", KEXEC_MOD_NAME, KEXEC_MOD_VERSION);

    /* Resolve unexported kernel symbols first */
    ret = kexec_mod_resolve_symbols();
    if (ret) {
        kexec_mod_err("Failed to resolve kernel symbols\n");
        return ret;
    }

    /* Allocate device number */
    ret = alloc_chrdev_region(&kexec_mod_dev, 0, 1, KEXEC_MOD_NAME);
    if (ret < 0) {
        kexec_mod_err("Failed to allocate device number\n");
        return ret;
    }

    /* Create device class */
    kexec_mod_class = class_create(THIS_MODULE, KEXEC_MOD_NAME);
    if (IS_ERR(kexec_mod_class)) {
        ret = PTR_ERR(kexec_mod_class);
        kexec_mod_err("Failed to create device class\n");
        goto err_unregister_chrdev;
    }

    /* Initialize and add character device */
    cdev_init(&kexec_mod_cdev, &kexec_mod_fops);
    kexec_mod_cdev.owner = THIS_MODULE;
    ret = cdev_add(&kexec_mod_cdev, kexec_mod_dev, 1);
    if (ret < 0) {
        kexec_mod_err("Failed to add character device\n");
        goto err_destroy_class;
    }

    /* Create device node */
    dev = device_create(kexec_mod_class, NULL, kexec_mod_dev, NULL,
                        KEXEC_MOD_NAME);
    if (IS_ERR(dev)) {
        ret = PTR_ERR(dev);
        kexec_mod_err("Failed to create device\n");
        goto err_cdev_del;
    }

    kexec_mod_info("Device created: /dev/%s\n", KEXEC_MOD_NAME);
    kexec_mod_info("Module loaded successfully\n");

    return 0;

err_cdev_del:
    cdev_del(&kexec_mod_cdev);
err_destroy_class:
    class_destroy(kexec_mod_class);
err_unregister_chrdev:
    unregister_chrdev_region(kexec_mod_dev, 1);
    return ret;
}

static void __exit kexec_mod_exit(void)
{
    kexec_mod_info("Unloading module\n");

    /* Free any loaded image */
    if (kexec_mod_image) {
        kexec_mod_image_free(kexec_mod_image);
        kexec_mod_image = NULL;
    }

    /* Cleanup device */
    device_destroy(kexec_mod_class, kexec_mod_dev);
    cdev_del(&kexec_mod_cdev);
    class_destroy(kexec_mod_class);
    unregister_chrdev_region(kexec_mod_dev, 1);

    kexec_mod_info("Module unloaded\n");
}

module_init(kexec_mod_init);
module_exit(kexec_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kexec Module Author");
MODULE_DESCRIPTION("Standalone Kexec Module for Android Kernel 5.10");
MODULE_VERSION(KEXEC_MOD_VERSION);
