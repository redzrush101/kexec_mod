// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_mod_load - Userspace tool for loading and executing kernels
 *
 * Usage:
 *   kexec_mod_load <kernel_image> [--entry <addr>] [--exec]
 *
 * Compile with:
 *   aarch64-linux-android-gcc -o kexec_mod_load kexec_mod_load.c -static
 *
 * Or with NDK:
 *   $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang \
 *       -o kexec_mod_load kexec_mod_load.c -static
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <errno.h>

/* Include our API header */
#include "../kexec_mod_user.h"

/* Default load address for ARM64 kernels */
#define DEFAULT_LOAD_ADDRESS    0x80080000UL
#define DEFAULT_ENTRY_OFFSET    0x0

/* Print usage */
static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <kernel_image> [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --load <addr>   Load address (default: 0x%lx)\n", DEFAULT_LOAD_ADDRESS);
    fprintf(stderr, "  --entry <addr>  Entry point (default: load address)\n");
    fprintf(stderr, "  --exec          Execute immediately after loading\n");
    fprintf(stderr, "  --status        Show current status\n");
    fprintf(stderr, "  --unload        Unload current image\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s Image --load 0x80080000 --exec\n", prog);
    fprintf(stderr, "  %s --status\n", prog);
}

/* Read entire file into memory */
static void *read_file(const char *path, size_t *size_out)
{
    FILE *fp;
    struct stat st;
    void *buf;
    size_t read_size;

    if (stat(path, &st) < 0) {
        perror("stat");
        return NULL;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        perror("fopen");
        return NULL;
    }

    buf = malloc(st.st_size);
    if (!buf) {
        perror("malloc");
        fclose(fp);
        return NULL;
    }

    read_size = fread(buf, 1, st.st_size, fp);
    fclose(fp);

    if (read_size != (size_t)st.st_size) {
        fprintf(stderr, "Short read: %zu of %ld\n", read_size, st.st_size);
        free(buf);
        return NULL;
    }

    *size_out = st.st_size;
    return buf;
}



/* Execute loaded kernel */
static int do_exec(int fd)
{
    int ret;

    printf("Executing kexec...\n");
    printf("*** POINT OF NO RETURN ***\n");
    
    sync();  /* Sync filesystems */
    
    ret = ioctl(fd, KEXEC_MOD_IOCTL_EXEC);
    
    /* Should not return on success */
    perror("ioctl(KEXEC_MOD_IOCTL_EXEC)");
    return -1;
}

/* Show status */
static int do_status(int fd)
{
    int status = 0;
    int ret;

    ret = ioctl(fd, KEXEC_MOD_IOCTL_STATUS, &status);
    if (ret < 0) {
        perror("ioctl(KEXEC_MOD_IOCTL_STATUS)");
        return -1;
    }

    printf("Kexec module status: %s\n", 
           status ? "Image loaded" : "No image loaded");
    return 0;
}

/* Unload image */
static int do_unload(int fd)
{
    int ret;

    ret = ioctl(fd, KEXEC_MOD_IOCTL_UNLOAD);
    if (ret < 0) {
        perror("ioctl(KEXEC_MOD_IOCTL_UNLOAD)");
        return -1;
    }

    printf("Image unloaded\n");
    return 0;
}

/* Load a kernel image with optional DTB */
static int do_load(int fd, const char *kernel_path, const char *dtb_path,
                   unsigned long load_addr, unsigned long entry_addr)
{
    void *kernel_data = NULL;
    void *dtb_data = NULL;
    size_t kernel_size = 0;
    size_t dtb_size = 0;
    struct kexec_mod_segment segments[2]; /* 0=Kernel, 1=DTB */
    struct kexec_mod_load_args args;
    unsigned long dtb_mem = 0;
    int nr_segments = 0;
    int ret = -1;

    printf("Loading kernel: %s\n", kernel_path);
    printf("  Load address: 0x%lx\n", load_addr);
    printf("  Entry point:  0x%lx\n", entry_addr);

    /* 1. Read kernel file */
    kernel_data = read_file(kernel_path, &kernel_size);
    if (!kernel_data) {
        fprintf(stderr, "Failed to read kernel file\n");
        goto out;
    }
    printf("  Kernel size:  %zu bytes\n", kernel_size);

    /* Setup Kernel Segment */
    segments[0].buf = kernel_data;
    segments[0].bufsz = kernel_size;
    segments[0].mem = load_addr;
    /* Align to 2MB just in case, typically page alignment (4K) is enough */
    segments[0].memsz = (kernel_size + 0x200000) & ~0x1FFFFFUL;
    if (segments[0].memsz < kernel_size) segments[0].memsz = kernel_size;
    nr_segments++;

    /* 2. Handle DTB */
    if (dtb_path) {
        printf("Loading DTB:    %s\n", dtb_path);
        dtb_data = read_file(dtb_path, &dtb_size);
        if (!dtb_data) {
             fprintf(stderr, "Failed to read DTB file\n");
             goto out;
        }
        printf("  DTB size:     %zu bytes\n", dtb_size);
        
        /* Place DTB after kernel, aligned to 2MB to be safe */
        dtb_mem = segments[0].mem + segments[0].memsz;
        /* Ensure it's at least 128MB aligned if possible? No, just ensure no overlap. */
        
        segments[1].buf = dtb_data;
        segments[1].bufsz = dtb_size;
        segments[1].mem = dtb_mem;
        segments[1].memsz = (dtb_size + 4095) & ~4095UL;
        nr_segments++;
        
        printf("  DTB address:  0x%lx\n", dtb_mem);
    } else {
        printf("No DTB specified. Using 0 (legacy/appended DTB mode)\n");
    }

    /* Set up load arguments */
    memset(&args, 0, sizeof(args));
    args.entry = entry_addr;
    args.nr_segments = nr_segments;
    args.segments = segments;
    args.flags = 0;
    args.dtb_mem = dtb_mem;

    /* Call ioctl */
    printf("Calling KEXEC_MOD_IOCTL_LOAD...\n");
    ret = ioctl(fd, KEXEC_MOD_IOCTL_LOAD, &args);
    
    if (ret < 0) {
        perror("ioctl(KEXEC_MOD_IOCTL_LOAD)");
    } else {
        printf("Kernel loaded successfully!\n");
        ret = 0;
    }

out:
    free(kernel_data);
    free(dtb_data);
    return ret;
}

int main(int argc, char *argv[])
{
    int fd;
    int i;
    const char *kernel_path = NULL;
    const char *dtb_path = NULL;
    unsigned long load_addr = DEFAULT_LOAD_ADDRESS;
    unsigned long entry_addr = 0;
    int do_exec_flag = 0;
    int status_only = 0;
    int unload_only = 0;
    int ret = 0;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--load") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for --load\n");
                return 1;
            }
            load_addr = strtoul(argv[i], NULL, 0);
        } else if (strcmp(argv[i], "--entry") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for --entry\n");
                return 1;
            }
            entry_addr = strtoul(argv[i], NULL, 0);
        } else if (strcmp(argv[i], "--dtb") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for --dtb\n");
                return 1;
            }
            dtb_path = argv[i];
        } else if (strcmp(argv[i], "--exec") == 0) {
            do_exec_flag = 1;
        } else if (strcmp(argv[i], "--status") == 0) {
            status_only = 1;
        } else if (strcmp(argv[i], "--unload") == 0) {
            unload_only = 1;
        } else if (argv[i][0] != '-') {
            kernel_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* Check for default DTB path if not specified */
    if (!dtb_path && !status_only && !unload_only) {
        struct stat st;
        if (stat("/sys/firmware/fdt", &st) == 0) {
            dtb_path = "/sys/firmware/fdt";
            printf("Found system DTB at %s, using it.\n", dtb_path);
        }
    }

    /* Default entry to load address */
    if (entry_addr == 0)
        entry_addr = load_addr;

    /* Open device */
    fd = open(KEXEC_MOD_DEVICE, O_RDWR);
    if (fd < 0) {
        perror("open(" KEXEC_MOD_DEVICE ")");
        fprintf(stderr, "Make sure kexec_mod module is loaded\n");
        return 1;
    }

    /* Handle status-only request */
    if (status_only) {
        ret = do_status(fd);
        close(fd);
        return ret ? 1 : 0;
    }

    /* Handle unload request */
    if (unload_only) {
        ret = do_unload(fd);
        close(fd);
        return ret ? 1 : 0;
    }

    /* Need kernel path for load */
    if (!kernel_path) {
        fprintf(stderr, "No kernel image specified\n");
        usage(argv[0]);
        close(fd);
        return 1;
    }

    /* Load kernel */
    ret = do_load(fd, kernel_path, dtb_path, load_addr, entry_addr);
    if (ret < 0) {
        close(fd);
        return 1;
    }

    /* Execute if requested */
    if (do_exec_flag) {
        ret = do_exec(fd);
        /* Should not reach here on success */
        close(fd);
        return 1;
    }

    close(fd);
    printf("Done. Use --exec to execute, or run again with --exec\n");
    return 0;
}
