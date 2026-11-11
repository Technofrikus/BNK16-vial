#!/bin/bash
# Build the BNK16 UF2 bootloader and firmware in the nookcraftlab/riscv Docker image.
# Usage: docker run --rm --platform linux/amd64 -v "<repo root>:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh
#
# Output in firmware/ch585_v2/build-bnk16/:
#   bnk16.uf2              firmware for drag-and-drop onto the BNK16BOOT drive
#   bnk16_full.bin         bootloader + firmware, for wchisp
#   bnk16.bin/.hex/.elf    firmware only (linked at 0x8000)
#   bnk16_bootloader.bin   bootloader only
# Test build with short idle stages (15/45/90 s): add -e BNK16_TEST_TIMERS=1 to the docker run call.
# Extra compiler defines for test variants: -e BNK16_DEFS="-DFOO -DBAR=0" (see rules.cmake).
# Another BLE library (folder with libCH58xBLE.a and its headers): -e BNK16_BLE_LIB_DIR=/workspace/...
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
    -DCMAKE_BUILD_TYPE=Release -DBNK16_TEST_TIMERS="${BNK16_TEST_TIMERS:-0}" \
    -DBNK16_DEFS="${BNK16_DEFS:-}" \
    -DBNK16_BLE_LIB_DIR="${BNK16_BLE_LIB_DIR:-}"
ninja

echo "=== images"
APP_START=$(sed -n 's/^set(BNK16_APP_START *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
FAMILY=$(sed -n 's/^set(BNK16_UF2_FAMILY *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
python3 "$ROOT/bootloader/make_images.py" bnk16_bootloader.bin bnk16.bin "$APP_START" "$FAMILY" bnk16

ls -la *.uf2 *.bin *.hex *.elf
