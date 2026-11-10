#!/bin/bash
# Build the BNK16 sleep test firmwares (tests/sleeptest) in the nookcraftlab/riscv Docker image.
# Usage: docker run --rm --platform linux/amd64 -v "<repo root>:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-sleeptest.sh
#
# Output in firmware/ch585_v2/build-sleeptest/: sleeptest-<n>-<variant>.uf2, for the BNK16BOOT drive
set -e
git config --global --add safe.directory '*'
GCC_PATH="/opt/Toolchain/RISC-V Embedded GCC12/bin"
ROOT=/workspace/firmware/ch585_v2
OUT=$ROOT/build-sleeptest

rm -rf "$OUT" && mkdir -p "$OUT"
cmake -S "$ROOT/tests/sleeptest" -B "$OUT" -G Ninja \
    -DTOOLCHAIN_PREFIX=riscv-wch-elf -DADDITIONAL_TOOLCHAIN_PATH="$GCC_PATH/"
ninja -C "$OUT"

APP_START=$(sed -n 's/^set(BNK16_APP_START *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
FAMILY=$(sed -n 's/^set(BNK16_UF2_FAMILY *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
cd "$OUT"
for bin in sleeptest-*.bin; do
    python3 "$ROOT/tests/sleeptest/make_uf2.py" "$bin" "$APP_START" "$FAMILY" "${bin%.bin}.uf2"
done
ls -la *.uf2
