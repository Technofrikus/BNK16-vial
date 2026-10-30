#!/bin/bash
# Build the BNK16 target in the nookcraftlab/riscv Docker image.
# Usage: docker run --rm --platform linux/amd64 -v "<repo root>:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh
set -e
git config --global --add safe.directory '*'
GCC_PATH="/opt/Toolchain/RISC-V Embedded GCC12/bin"
cd /workspace/firmware/ch585_v2
rm -rf build-bnk16 && mkdir -p build-bnk16 && cd build-bnk16
cmake .. -G Ninja -Dkeyboard=bnk16 -Dkeymap=default \
    -DTOOLCHAIN_PREFIX=riscv-wch-elf -DADDITIONAL_TOOLCHAIN_PATH="$GCC_PATH/" \
    -DCMAKE_BUILD_TYPE=Release
ninja
ls -la *.hex *.bin *.elf
