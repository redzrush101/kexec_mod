# kexec_mod Development & Testing Setup

## Status
Tested on GitHub kernel. Started but not all issues are fixed yet.

## Cuttlefish Setup
Based on the official guide: [Get Started with Cuttlefish](https://source.android.com/docs/devices/cuttlefish/get-started)

**Environment Tested:**
- **Build:** Android 14 (kernel 6.1.157)
- **Host:** ARM64 Server My server specs was (13 cores 32GB RAM)
- **Virtualization:** No KVM support (requires patches/wrapper)
- **Note:** Setup failed on older builds (e.g., Android 12).

## Launch Instructions (No KVM)

Since the ARM64 server lacks KVM support, I use `qemu_cli` with a custom wrapper to enforce TCG and modify arguments.

### 1. Launch Script
Run Cuttlefish with the following command:

```bash
HOME=$(pwd) ./bin/launch_cvd \
  -vm_manager qemu_cli \
  -report_anonymous_usage_stats=n \
  --start_webrtc=false \
  -qemu_binary_dir=$HOME/qemu-wrapper \
  -memory_mb=8192 \
  -cpus=16
```

### 2. QEMU Wrapper
Create the wrapper script at `$HOME/qemu-wrapper/qemu-system-aarch64`. This script forces TCG mode and removes unsupported options.

```bash
#!/bin/bash
{
    echo "=== Called at $(date) ==="
    echo "Original args: $*"

    new_cmd="$*"

    # Force TCG without thread parameter (not supported in older QEMU)
    new_cmd="${new_cmd//accel=kvm/accel=tcg}"

    # Use max CPU features for TCG
    new_cmd="${new_cmd//-cpu host/-cpu max}"

    # Remove problematic options
    new_cmd="${new_cmd//scsi=off,/}"
    new_cmd="${new_cmd//,scsi=off/}"
    new_cmd="${new_cmd//-device AC97/}"

    # Enable highmem
    if [[ "$new_cmd" =~ -machine\ +([^\ ]+) ]]; then
        old_machine="${BASH_REMATCH[1]}"
        if [[ ! "$old_machine" =~ highmem ]]; then
            new_machine="${old_machine},highmem=on"
            new_cmd="${new_cmd//-machine $old_machine/-machine $new_machine}"
        fi
    fi

    echo "Modified args: $new_cmd"
} >> /tmp/qemu-debug.log

eval exec /usr/bin/qemu-system-aarch64 $new_cmd
```
*Make sure to `chmod +x` the wrapper.*

## Kernel Modifications

To enable `kexec_mod` functionality, the following patches were applied to the **child kernel** (the kernel we are going to execute):

### Disable GKI Symbol Protection
Modified `kernel/module/gki_module.c` to disable GKI protection checks.
- **gki_is_module_protected_export**: Forced to return `false` (allowing all exports).
- **gki_is_module_unprotected_symbol**: Forced to return `true` (allowing all access).

### SELinux
Added `androidboot.selinux=permissive` to the kernel commandline (or bootconfig) to avoid permission denials during testing.
