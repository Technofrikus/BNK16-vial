#!/bin/bash
# Build the BNK16 BLE test firmwares (tests/bletest, test B) in the nookcraftlab/riscv Docker image.
# Usage: docker run --rm --platform linux/amd64 -v "<repo root>:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bletest.sh
#
# Output in firmware/ch585_v2/build-bletest/: bletest-<variant>.uf2, for the BNK16BOOT drive
set -e
git config --global --add safe.directory '*'
GCC_PATH="/opt/Toolchain/RISC-V Embedded GCC12/bin"
ROOT=/workspace/firmware/ch585_v2
# BLETEST_EVT=web: build against tests/bletest/evt-web (copy of the WCH website EVT) into build-bletest-web/
if [ "${BLETEST_EVT:-}" = web ]; then
    EVT_DIR=$ROOT/tests/bletest/evt-web
    OUT=$ROOT/build-bletest-web
else
    EVT_DIR=$ROOT/tests/bletest/evt
    OUT=$ROOT/build-bletest
fi

rm -rf "$OUT" && mkdir -p "$OUT"
cmake -S "$ROOT/tests/bletest" -B "$OUT" -G Ninja \
    -DTOOLCHAIN_PREFIX=riscv-wch-elf -DADDITIONAL_TOOLCHAIN_PATH="$GCC_PATH/" -DEVT_DIR="$EVT_DIR" \
    -DBLETEST_VARIANTS="${BLETEST_VARIANTS:-}" -DBLETEST_REGDUMP="${BLETEST_REGDUMP:-0}"
ninja -C "$OUT"

APP_START=$(sed -n 's/^set(BNK16_APP_START *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
FAMILY=$(sed -n 's/^set(BNK16_UF2_FAMILY *\(0x[0-9A-Fa-f]*\)).*/\1/p' "$ROOT/bootloader/layout.cmake")
cd "$OUT"
for bin in bletest-*.bin; do
    python3 "$ROOT/tests/sleeptest/make_uf2.py" "$bin" "$APP_START" "$FAMILY" "${bin%.bin}.uf2"
done
ls -la *.uf2
