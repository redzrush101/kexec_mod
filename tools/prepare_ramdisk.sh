#!/bin/bash
# prepare_ramdisk.sh - Prepare combined ramdisk with bootconfig for kexec
#
# Usage: ./prepare_ramdisk.sh [output.lz4]
# 
# Expects vendor_boot.img and init_boot.img in current directory.
# This script properly decompresses, combines, and recompresses the ramdisks.

set -e

OUTPUT="${1:-ramdisk_kexec.lz4}"

VENDOR_BOOT_IMG="${VENDOR_BOOT_IMG:-vendor_boot.img}"
INIT_BOOT_IMG="${INIT_BOOT_IMG:-init_boot.img}"

echo "=== Kexec Ramdisk Preparation ==="
echo ""

# Check for required files
for img in "$VENDOR_BOOT_IMG" "$INIT_BOOT_IMG"; do
    if [ ! -f "$img" ]; then
        echo "ERROR: Required file not found: $img"
        exit 1
    fi
done

# Check for required tools
for tool in unpack_bootimg lz4; do
    if ! command -v $tool &> /dev/null; then
        echo "ERROR: $tool not found"
        echo "Install with: apt install lz4 / pip3 install mkbootimg"
        exit 1
    fi
done

WORK_DIR=$(mktemp -d)
trap "rm -rf $WORK_DIR" EXIT

echo "Step 1: Extracting vendor_boot.img..."
unpack_bootimg --boot_img "$VENDOR_BOOT_IMG" --out "$WORK_DIR/vendor_boot" 2>/dev/null

if [ -f "$WORK_DIR/vendor_boot/vendor_ramdisk00" ]; then
    VENDOR_RAMDISK="$WORK_DIR/vendor_boot/vendor_ramdisk00"
else
    VENDOR_RAMDISK="$WORK_DIR/vendor_boot/vendor_ramdisk"
fi
echo "  Found: $VENDOR_RAMDISK ($(stat -c%s "$VENDOR_RAMDISK") bytes)"

BOOTCONFIG_FILE="$WORK_DIR/vendor_boot/bootconfig"

echo ""
echo "Step 2: Extracting init_boot.img..."
unpack_bootimg --boot_img "$INIT_BOOT_IMG" --out "$WORK_DIR/init_boot" 2>/dev/null
GENERIC_RAMDISK="$WORK_DIR/init_boot/ramdisk"
echo "  Found: $GENERIC_RAMDISK ($(stat -c%s "$GENERIC_RAMDISK") bytes)"

echo ""
echo "Step 3: Decompressing ramdisks..."

# Decompress vendor ramdisk (LZ4 -> CPIO)
lz4 -d -f "$VENDOR_RAMDISK" "$WORK_DIR/vendor.cpio" 2>/dev/null || {
    # Try as raw LZ4 block format
    lz4 -d -f --no-frame-crc "$VENDOR_RAMDISK" "$WORK_DIR/vendor.cpio" 2>/dev/null || {
        echo "  Note: Trying legacy LZ4 decode..."
        # Use Python for legacy format
        python3 << PYEOF
import lz4.frame
import lz4.block
import sys

try:
    with open("$VENDOR_RAMDISK", 'rb') as f:
        data = f.read()
    # Try frame format first
    try:
        decompressed = lz4.frame.decompress(data)
    except:
        # Try block format (legacy)
        decompressed = lz4.block.decompress(data)
    with open("$WORK_DIR/vendor.cpio", 'wb') as f:
        f.write(decompressed)
    print(f"  Decompressed vendor ramdisk: {len(decompressed)} bytes")
except Exception as e:
    print(f"ERROR: Failed to decompress vendor ramdisk: {e}")
    sys.exit(1)
PYEOF
    }
}
VENDOR_CPIO_SIZE=$(stat -c%s "$WORK_DIR/vendor.cpio" 2>/dev/null || echo 0)
echo "  Vendor CPIO: $VENDOR_CPIO_SIZE bytes"

# Decompress generic ramdisk
lz4 -d -f "$GENERIC_RAMDISK" "$WORK_DIR/generic.cpio" 2>/dev/null || {
    python3 << PYEOF
import lz4.frame
import lz4.block

try:
    with open("$GENERIC_RAMDISK", 'rb') as f:
        data = f.read()
    try:
        decompressed = lz4.frame.decompress(data)
    except:
        decompressed = lz4.block.decompress(data)
    with open("$WORK_DIR/generic.cpio", 'wb') as f:
        f.write(decompressed)
    print(f"  Decompressed generic ramdisk: {len(decompressed)} bytes")
except Exception as e:
    print(f"ERROR: Failed to decompress generic ramdisk: {e}")
    import sys
    sys.exit(1)
PYEOF
}
GENERIC_CPIO_SIZE=$(stat -c%s "$WORK_DIR/generic.cpio" 2>/dev/null || echo 0)
echo "  Generic CPIO: $GENERIC_CPIO_SIZE bytes"

echo ""
echo "Step 4: Combining CPIO archives..."
cat "$WORK_DIR/generic.cpio" "$WORK_DIR/vendor.cpio" > "$WORK_DIR/combined.cpio"

# Check if fstab file exists in current directory and inject it
FSTAB_FILE=""
for suffix in cf.f2fs.hctr2 cf.f2fs.cts cf.ext4.hctr2 cf.ext4.cts; do
    if [ -f "fstab.$suffix" ]; then
        FSTAB_FILE="fstab.$suffix"
        break
    fi
done

# If not found locally, try to pull from device via adb
if [ -z "$FSTAB_FILE" ] && command -v adb &> /dev/null; then
    echo "  No local fstab found, trying to pull from device..."
    for suffix in cf.f2fs.hctr2 cf.f2fs.cts cf.ext4.hctr2 cf.ext4.cts; do
        if adb pull "/vendor/etc/fstab.$suffix" "fstab.$suffix" 2>/dev/null; then
            FSTAB_FILE="fstab.$suffix"
            echo "  Pulled fstab.$suffix from device"
            break
        fi
    done
fi

if [ -n "$FSTAB_FILE" ]; then
    echo "  Injecting $FSTAB_FILE into ramdisk at multiple locations..."
    rm -rf "$WORK_DIR/fstab_inject"
    
    # Create fstab at BOTH /system/etc/ AND /first_stage_ramdisk/system/etc/
    # We're not sure which one Android init expects, so cover both cases
    mkdir -p "$WORK_DIR/fstab_inject/system/etc"
    mkdir -p "$WORK_DIR/fstab_inject/first_stage_ramdisk/system/etc"
    cp "$FSTAB_FILE" "$WORK_DIR/fstab_inject/system/etc/"
    cp "$FSTAB_FILE" "$WORK_DIR/fstab_inject/first_stage_ramdisk/system/etc/"
    
    # Fix permissions
    chmod -R 755 "$WORK_DIR/fstab_inject"
    find "$WORK_DIR/fstab_inject" -type f -exec chmod 644 {} \;
    
    # Create CPIO using find to ensure all entries are included
    (cd "$WORK_DIR/fstab_inject" && find . -print0 | cpio -0 -o -H newc 2>/dev/null) >> "$WORK_DIR/combined.cpio"
    echo "  Injected fstab at /system/etc/$FSTAB_FILE"
    echo "  Injected fstab at /first_stage_ramdisk/system/etc/$FSTAB_FILE"
else
    echo "  WARNING: No fstab file found (tried local and adb pull)"
fi

COMBINED_SIZE=$(stat -c%s "$WORK_DIR/combined.cpio")
echo "  Combined CPIO: $COMBINED_SIZE bytes"

echo ""
echo "Step 5: Recompressing as single LZ4..."
lz4 -l -9 -f "$WORK_DIR/combined.cpio" "$WORK_DIR/combined.lz4"
LZ4_SIZE=$(stat -c%s "$WORK_DIR/combined.lz4")
echo "  Compressed size: $LZ4_SIZE bytes"

echo ""
echo "Step 6: Preparing bootconfig..."

# Check for local bootconfig.txt first (from /proc/bootconfig format)
LOCAL_BOOTCONFIG="$(dirname "$0")/../../../bootconfig.txt"
if [ ! -f "$LOCAL_BOOTCONFIG" ]; then
    LOCAL_BOOTCONFIG="./bootconfig.txt"
fi

if [ -f "$LOCAL_BOOTCONFIG" ]; then
    echo "  Using local bootconfig file: $LOCAL_BOOTCONFIG"
    cp "$LOCAL_BOOTCONFIG" "$WORK_DIR/bootconfig_final"
    # Convert from /proc format (key = "value") to bootconfig format (key=value)
    sed -i 's/ = /=/g; s/"//g' "$WORK_DIR/bootconfig_final"
elif adb shell "cat /proc/bootconfig" > "$WORK_DIR/bootconfig_final" 2>/dev/null; then
    echo "  Pulled complete bootconfig from running device"
    sed -i 's/ = /=/g; s/"//g' "$WORK_DIR/bootconfig_final"
elif [ -f "$BOOTCONFIG_FILE" ]; then
    cp "$BOOTCONFIG_FILE" "$WORK_DIR/bootconfig_final"
    echo "  Using vendor_boot bootconfig file: $BOOTCONFIG_FILE"
else
    touch "$WORK_DIR/bootconfig_final"
    echo "  WARNING: No bootconfig found, creating minimal one"
fi

# Always ensure force_normal_boot=1 (may need to override existing value)
sed -i '/androidboot.force_normal_boot/d' "$WORK_DIR/bootconfig_final" 2>/dev/null || true
echo "androidboot.force_normal_boot=1" >> "$WORK_DIR/bootconfig_final"

# Ensure trailing newline
sed -i -e '$a\' "$WORK_DIR/bootconfig_final" 2>/dev/null || true

echo "  Bootconfig contents:"
sed 's/^/    /' "$WORK_DIR/bootconfig_final"

echo ""
echo "Step 7: Creating bootconfig trailer and final ramdisk..."

# Pad combined.lz4 to 4-byte alignment, add bootconfig + trailer
export WORK_DIR
export OUTPUT
python3 << 'PYEOF'
import struct
import os

work_dir = os.environ['WORK_DIR']
output = os.environ['OUTPUT']

# Read compressed ramdisk
with open(f'{work_dir}/combined.lz4', 'rb') as f:
    ramdisk = f.read()

# Pad to 4-byte alignment
pad_len = (4 - (len(ramdisk) % 4)) % 4
ramdisk += b'\x00' * pad_len

# Read bootconfig
with open(f'{work_dir}/bootconfig_final', 'rb') as f:
    bootconfig = f.read()

# Calculate checksum - IMPORTANT: kernel uses simple byte sum, NOT CRC32!
# See include/linux/bootconfig.h: xbc_calc_checksum()
size = len(bootconfig)
checksum = sum(bootconfig) & 0xffffffff  # Simple byte addition

print(f"  Bootconfig size: {size} bytes")
print(f"  Bootconfig checksum (byte sum): 0x{checksum:08x}")

# Create trailer: size (LE u32) + checksum (LE u32) + magic
trailer = struct.pack('<I', size) + struct.pack('<I', checksum) + b'#BOOTCONFIG\n'

# Combine everything
final = ramdisk + bootconfig + trailer

with open(output, 'wb') as f:
    f.write(final)

print(f"  Final size: {len(final)} bytes")
PYEOF

FINAL_SIZE=$(stat -c%s "$OUTPUT")
echo ""
echo "=== Verification ==="
echo "Last 32 bytes:"
hexdump -C "$OUTPUT" | tail -3

# Verify LZ4 header
echo ""
echo "LZ4 header (first 8 bytes):"
hexdump -C "$OUTPUT" | head -1

echo ""
echo "=== Done! ==="
echo "Output: $OUTPUT ($FINAL_SIZE bytes)"
echo ""
echo "Run kexec with:"
echo "  ./kexec_mod_load Image --load 0x80000000 --initrd $OUTPUT --exec"
