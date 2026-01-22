// SPDX-License-Identifier: GPL-2.0
/*
 * kexec_mod_load - Userspace tool for loading and executing kernels
 *
 * Usage:
 *   kexec_mod_load <kernel_image> [--load <addr>] [--initrd <path>] [--cmdline <args>] [--exec]
 *
 * Compile with:
 *   aarch64-linux-android-gcc -o kexec_mod_load kexec_mod_load.c -static
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
#include <stdint.h>

/* Include our API header */
#include "../kexec_mod_user.h"

/* Default load address for ARM64 kernels */
#define DEFAULT_LOAD_ADDRESS    0x80080000UL
#define DEFAULT_ENTRY_OFFSET    0x0

/* FDT definitions for DTB manipulation */
#define FDT_MAGIC       0xd00dfeed
#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

/* FDT header structure */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static inline uint32_t fdt32_to_cpu(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint32_t cpu_to_fdt32(uint32_t x) {
    return __builtin_bswap32(x);
}

static inline uint64_t cpu_to_fdt64(uint64_t x) {
    return __builtin_bswap64(x);
}

/* Print usage */
static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <kernel_image> [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --load <addr>     Load address (default: 0x%lx)\n", DEFAULT_LOAD_ADDRESS);
    fprintf(stderr, "  --entry <addr>    Entry point (default: load address)\n");
    fprintf(stderr, "  --dtb <path>      Device tree blob (default: /sys/firmware/fdt)\n");
    fprintf(stderr, "  --initrd <path>   Initial ramdisk (default: none)\n");
    fprintf(stderr, "  --cmdline <args>  Kernel command line (default: /proc/cmdline)\n");
    fprintf(stderr, "  --reuse-cmdline   Reuse current kernel's command line\n");
    fprintf(stderr, "  --exec            Execute immediately after loading\n");
    fprintf(stderr, "  --status          Show current status\n");
    fprintf(stderr, "  --unload          Unload current image\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s Image --load 0x60000000 --reuse-cmdline --exec\n", prog);
    fprintf(stderr, "  %s Image --load 0x80080000 --initrd ramdisk.img --exec\n", prog);
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

/* Read /proc/cmdline */
static char *read_cmdline(void)
{
    FILE *fp;
    static char buf[4096];
    size_t len;

    fp = fopen("/proc/cmdline", "r");
    if (!fp) {
        perror("fopen(/proc/cmdline)");
        return NULL;
    }

    if (!fgets(buf, sizeof(buf), fp)) {
        perror("fgets");
        fclose(fp);
        return NULL;
    }
    fclose(fp);

    /* Remove trailing newline */
    len = strlen(buf);
    if (len > 0 && buf[len-1] == '\n')
        buf[len-1] = '\0';

    return buf;
}

/* Find string offset in DTB string table */
static int fdt_find_string(void *fdt, const char *str)
{
    struct fdt_header *header = fdt;
    uint32_t totalsize = fdt32_to_cpu(header->totalsize);
    uint32_t off_strings = fdt32_to_cpu(header->off_dt_strings);
    uint32_t size_strings = fdt32_to_cpu(header->size_dt_strings);
    char *strings = (char *)fdt + off_strings;
    uint32_t i;
    
    for (i = 0; i < size_strings; ) {
        if (strcmp(strings + i, str) == 0)
            return i;
        i += strlen(strings + i) + 1;
    }
    return -1;
}

/* Add a string to the DTB string table */
static int fdt_add_string(void *fdt, size_t fdt_size, const char *str)
{
    struct fdt_header *header = fdt;
    uint32_t off_strings = fdt32_to_cpu(header->off_dt_strings);
    uint32_t size_strings = fdt32_to_cpu(header->size_dt_strings);
    uint32_t totalsize = fdt32_to_cpu(header->totalsize);
    size_t str_len = strlen(str) + 1;
    
    /* Check if we have room */
    if (totalsize + str_len > fdt_size) {
        fprintf(stderr, "No room to add string to DTB\n");
        return -1;
    }
    
    /* Copy string to end of string table */
    memcpy((char *)fdt + off_strings + size_strings, str, str_len);
    
    /* Update sizes */
    header->size_dt_strings = cpu_to_fdt32(size_strings + str_len);
    header->totalsize = cpu_to_fdt32(totalsize + str_len);
    
    return size_strings;
}

/* Append a string to bootargs in the chosen node */
static int patch_bootargs_append(void *fdt, size_t fdt_buf_size, const char *append_str)
{
    struct fdt_header *header = fdt;
    uint32_t off_struct = fdt32_to_cpu(header->off_dt_struct);
    uint32_t off_strings = fdt32_to_cpu(header->off_dt_strings);
    uint32_t size_struct = fdt32_to_cpu(header->size_dt_struct);
    uint32_t *ptr, *struct_end;
    char *strings;
    int in_chosen = 0;
    int depth = 0;
    
    strings = (char *)fdt + off_strings;
    ptr = (uint32_t *)((char *)fdt + off_struct);
    struct_end = (uint32_t *)((char *)fdt + off_struct + size_struct);
    
    while (ptr < struct_end) {
        uint32_t token = fdt32_to_cpu(*ptr);
        ptr++;
        
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)ptr;
            size_t name_len = strlen(name) + 1;
            ptr = (uint32_t *)(((uintptr_t)ptr + name_len + 3) & ~3);
            if (depth == 1 && strcmp(name, "chosen") == 0) {
                in_chosen = 1;
            }
            depth++;
            break;
        }
        case FDT_END_NODE:
            if (in_chosen && depth == 2) in_chosen = 0;
            depth--;
            break;
        case FDT_PROP: {
            uint32_t *len_ptr = ptr - 1;  /* Point back to len field - but we already incremented */
            /* Re-read from correct position */
            uint32_t len = fdt32_to_cpu(*ptr++);
            uint32_t nameoff = fdt32_to_cpu(*ptr++);
            char *value = (char *)ptr;
            uint32_t *value_start = ptr;
            ptr = (uint32_t *)(((uintptr_t)ptr + len + 3) & ~3);
            
            if (in_chosen) {
                const char *prop_name = strings + nameoff;
                if (strcmp(prop_name, "bootargs") == 0) {
                    /* Found bootargs - check if we can append in-place */
                    size_t current_len = strlen(value);
                    size_t append_len = strlen(append_str);
                    size_t new_len = current_len + 1 + append_len + 1;  /* space + append + null */
                    uint32_t old_aligned = (len + 3) & ~3;
                    uint32_t new_aligned = (new_len + 3) & ~3;
                    
                    if (new_aligned <= old_aligned) {
                        /* We have room - append in place */
                        value[current_len] = ' ';
                        memcpy(value + current_len + 1, append_str, append_len + 1);
                        /* Update length field (it's at value_start - 2) */
                        *(value_start - 2) = cpu_to_fdt32(new_len);
                        printf("  Appended to bootargs: %s\n", append_str);
                        return 0;
                    } else {
                        printf("  WARNING: Not enough space to append to bootargs (need %u, have %u)\n",
                               new_aligned, old_aligned);
                        /* Just print what we'd want to add */
                        printf("  Wanted to append: %s\n", append_str);
                        return 0;  /* Non-fatal */
                    }
                }
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return 0;
        default:
            return -1;
        }
    }
    return 0;
}

/* Find the /chosen node and UPDATE existing initrd properties in-place */
static int patch_dtb_chosen(void *fdt, size_t *fdt_size, size_t fdt_buf_size,
                            const char *cmdline,
                            unsigned long initrd_start, unsigned long initrd_end)
{
    struct fdt_header *header = fdt;
    uint32_t magic = fdt32_to_cpu(header->magic);
    uint32_t totalsize = fdt32_to_cpu(header->totalsize);
    uint32_t off_struct = fdt32_to_cpu(header->off_dt_struct);
    uint32_t off_strings = fdt32_to_cpu(header->off_dt_strings);
    uint32_t size_struct = fdt32_to_cpu(header->size_dt_struct);
    uint32_t *ptr, *struct_end;
    char *strings;
    int in_chosen = 0;
    int depth = 0;
    int found_start = 0, found_end = 0;
    
    if (magic != FDT_MAGIC) {
        fprintf(stderr, "Invalid DTB magic: 0x%08x\n", magic);
        return -1;
    }
    
    printf("  DTB valid, totalsize=%u\n", totalsize);
    
    /* Note: androidboot.force_normal_boot=1 is now in bootconfig, 
     * no need to patch DTB bootargs */
    
    if (!initrd_start || !initrd_end) {
        printf("  No initrd to patch\n");
        *fdt_size = totalsize;
        return 0;
    }
    
    printf("  Patching initrd: 0x%lx - 0x%lx\n", initrd_start, initrd_end);
    
    strings = (char *)fdt + off_strings;
    ptr = (uint32_t *)((char *)fdt + off_struct);
    struct_end = (uint32_t *)((char *)fdt + off_struct + size_struct);
    
    /* 
     * Walk the structure to find the /chosen node 
     * and UPDATE existing initrd properties in-place
     */
    while (ptr < struct_end) {
        uint32_t token = fdt32_to_cpu(*ptr);
        ptr++;
        
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)ptr;
            size_t name_len = strlen(name) + 1;
            ptr = (uint32_t *)(((uintptr_t)ptr + name_len + 3) & ~3);  /* Align */
            
            if (depth == 1 && strcmp(name, "chosen") == 0) {
                in_chosen = 1;
                printf("  Found /chosen node\n");
            }
            depth++;
            break;
        }
        case FDT_END_NODE:
            if (in_chosen && depth == 2) {
                in_chosen = 0;
            }
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len = fdt32_to_cpu(*ptr++);
            uint32_t nameoff = fdt32_to_cpu(*ptr++);
            uint8_t *value = (uint8_t *)ptr;
            ptr = (uint32_t *)(((uintptr_t)ptr + len + 3) & ~3);  /* Skip value + align */
            
            /* Update existing initrd properties in-place */
            if (in_chosen) {
                const char *prop_name = strings + nameoff;
                
                if (strcmp(prop_name, "linux,initrd-start") == 0 && len == 8) {
                    /* Update the 64-bit value in-place */
                    uint64_t val64 = cpu_to_fdt64((uint64_t)initrd_start);
                    memcpy(value, &val64, 8);
                    printf("  Updated linux,initrd-start = 0x%lx\n", initrd_start);
                    found_start = 1;
                }
                else if (strcmp(prop_name, "linux,initrd-end") == 0 && len == 8) {
                    /* Update the 64-bit value in-place */
                    uint64_t val64 = cpu_to_fdt64((uint64_t)initrd_end);
                    memcpy(value, &val64, 8);
                    printf("  Updated linux,initrd-end = 0x%lx\n", initrd_end);
                    found_end = 1;
                }
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            goto done_walk;
        default:
            fprintf(stderr, "Unknown FDT token: 0x%x\n", token);
            return -1;
        }
    }
done_walk:

    if (found_start && found_end) {
        printf("  Successfully updated existing initrd properties\n");
        *fdt_size = totalsize;  /* Size unchanged - we only modified existing values */
        return 0;
    }
    
    if (!found_start || !found_end) {
        fprintf(stderr, "  WARNING: Could not find existing initrd properties to update\n");
        fprintf(stderr, "  found_start=%d, found_end=%d\n", found_start, found_end);
        fprintf(stderr, "  Initrd may not work. Consider using a DTB with pre-existing initrd properties.\n");
        /* Still return success - the kernel might boot but without initrd */
    }
    
    *fdt_size = totalsize;
    return 0;
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

/* Load a kernel image with optional DTB and initrd */
static int do_load(int fd, const char *kernel_path, const char *dtb_path,
                   const char *initrd_path, const char *cmdline,
                   unsigned long load_addr, unsigned long entry_addr)
{
    void *kernel_data = NULL;
    void *dtb_data = NULL;
    void *initrd_data = NULL;
    size_t kernel_size = 0;
    size_t dtb_size = 0;
    size_t dtb_buf_size = 0;
    size_t initrd_size = 0;
    struct kexec_mod_segment segments[3]; /* 0=Kernel, 1=DTB, 2=Initrd */
    struct kexec_mod_load_args args;
    unsigned long dtb_mem = 0;
    unsigned long initrd_mem = 0;
    unsigned long initrd_end = 0;
    unsigned long next_addr;
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
    /* Align to 2MB */
    segments[0].memsz = (kernel_size + 0x1FFFFF) & ~0x1FFFFFUL;
    nr_segments++;
    
    next_addr = segments[0].mem + segments[0].memsz;

    /* 2. Handle DTB */
    if (dtb_path) {
        printf("Loading DTB:    %s\n", dtb_path);
        dtb_data = read_file(dtb_path, &dtb_size);
        if (!dtb_data) {
             fprintf(stderr, "Failed to read DTB file\n");
             goto out;
        }
        printf("  DTB size:     %zu bytes\n", dtb_size);
        
        /* Allocate extra space for modifications */
        dtb_buf_size = dtb_size + 4096;
        dtb_data = realloc(dtb_data, dtb_buf_size);
        if (!dtb_data) {
            fprintf(stderr, "Failed to realloc DTB\n");
            goto out;
        }
        
        dtb_mem = next_addr;
        
        segments[nr_segments].buf = dtb_data;
        segments[nr_segments].bufsz = dtb_size;
        segments[nr_segments].mem = dtb_mem;
        segments[nr_segments].memsz = (dtb_size + 0xFFF) & ~0xFFFUL;
        nr_segments++;
        
        next_addr = dtb_mem + ((dtb_size + 0xFFF) & ~0xFFFUL);
        printf("  DTB address:  0x%lx\n", dtb_mem);
    }
    
    /* 3. Handle initrd */
    if (initrd_path) {
        printf("Loading initrd: %s\n", initrd_path);
        initrd_data = read_file(initrd_path, &initrd_size);
        if (!initrd_data) {
            fprintf(stderr, "Failed to read initrd file\n");
            goto out;
        }
        printf("  Initrd size:  %zu bytes\n", initrd_size);
        
        /* Align to page */
        initrd_mem = (next_addr + 0xFFF) & ~0xFFFUL;
        initrd_end = initrd_mem + initrd_size;
        
        segments[nr_segments].buf = initrd_data;
        segments[nr_segments].bufsz = initrd_size;
        segments[nr_segments].mem = initrd_mem;
        segments[nr_segments].memsz = (initrd_size + 0xFFF) & ~0xFFFUL;
        nr_segments++;
        
        printf("  Initrd addr:  0x%lx - 0x%lx\n", initrd_mem, initrd_end);
    }
    
    /* 4. Patch DTB with cmdline and initrd info if needed */
    if (dtb_data && (cmdline || initrd_mem)) {
        ret = patch_dtb_chosen(dtb_data, &dtb_size, dtb_buf_size,
                               cmdline, initrd_mem, initrd_end);
        if (ret < 0) {
            fprintf(stderr, "Warning: DTB patching failed\n");
            /* Continue anyway, firmware bootargs may work */
        }
        /* Update segment size in case DTB grew */
        segments[1].bufsz = dtb_size;
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
    free(initrd_data);
    return ret;
}

int main(int argc, char *argv[])
{
    int fd;
    int i;
    const char *kernel_path = NULL;
    const char *dtb_path = NULL;
    const char *initrd_path = NULL;
    const char *cmdline = NULL;
    unsigned long load_addr = DEFAULT_LOAD_ADDRESS;
    unsigned long entry_addr = 0;
    int do_exec_flag = 0;
    int status_only = 0;
    int unload_only = 0;
    int reuse_cmdline = 0;
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
        } else if (strcmp(argv[i], "--initrd") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for --initrd\n");
                return 1;
            }
            initrd_path = argv[i];
        } else if (strcmp(argv[i], "--cmdline") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Missing argument for --cmdline\n");
                return 1;
            }
            cmdline = argv[i];
        } else if (strcmp(argv[i], "--reuse-cmdline") == 0) {
            reuse_cmdline = 1;
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

    /* Get cmdline from current kernel if requested */
    if (reuse_cmdline && !cmdline) {
        cmdline = read_cmdline();
        if (cmdline) {
            printf("Reusing cmdline: %s\n", cmdline);
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
    ret = do_load(fd, kernel_path, dtb_path, initrd_path, cmdline,
                  load_addr, entry_addr);
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
