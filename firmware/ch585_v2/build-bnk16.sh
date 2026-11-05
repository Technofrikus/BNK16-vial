#!/bin/bash
# Build the BNK16 UF2 bootloader and firmware in the nookcraftlab/riscv Docker image.
# Usage: docker run --rm --platform linux/amd64 -v "<repo root>:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh
#
# Output in firmware/ch585_v2/build-bnk16/:
#   bnk16.uf2              firmware for drag-and-drop onto the BNK16BOOT drive
#   bnk16_full.bin         bootloader + firmware, for wchisp
#   bnk16.bin/.hex/.elf    firmware only (linked at 0x8000)
#   bnk16_bootloader.bin   bootloader only
set -e
git config --global --add safe.directory '*'
GCC_PATH="/opt/Toolchain/RISC-V Embedded GCC12/bin"
ROOT=/workspace/firmware/ch585_v2
OUT=$ROOT/build-bnk16

rm -rf "$OUT" && mkdir -p "$OUT/bootloader"

echo "=== bootloader"
cmake -S "$ROOT/bootloader" -B "$OUT/bootloader" -G Ninja \
    -DTOOLCHAIN_PREFIX=riscv-wch-elf -DADDITIONAL_TOOLCHAIN_PATH="$GCC_PATH/"
ninja -C "$OUT/bootloader"
cp "$OUT/bootloader/bnk16_bootloader.bin" "$OUT/"

echo "=== firmware"
cd "$OUT"
cmake .. -G Ninja -Dkeyboard=bnk16 -Dkeymap=vial \
    -DTOOLCHAIN_PREFIX=riscv-wch-elf -DADDITIONAL_TOOLCHAIN_PATH="$GCC_PATH/" \
    -DCMAKE_BUILD_TYPE=Release
ninja

echo "=== images"
APP_START=$(sed -n 's/^set(BNK16_APP_START *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
FAMILY=$(sed -n 's/^set(BNK16_UF2_FAMILY *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
python3 "$ROOT/bootloader/make_images.py" bnk16_bootloader.bin bnk16.bin "$APP_START" "$FAMILY" bnk16

ls -la *.uf2 *.bin *.hex *.elf
