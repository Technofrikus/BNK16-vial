#!/usr/bin/env python3
"""Turn the bootloader and firmware binaries into the release images.

  <out>.uf2        firmware as UF2, for drag-and-drop onto the BNK16BOOT drive
  <out>_full.bin   bootloader at 0x0 + firmware at app_start, for wchisp

Usage: make_images.py BOOTLOADER_BIN APP_BIN APP_START FAMILY_ID OUT_PREFIX
"""

import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
PAYLOAD = 256


def to_uf2(data: bytes, base: int, family: int) -> bytes:
    if len(data) % PAYLOAD:
        data += b"\xff" * (PAYLOAD - len(data) % PAYLOAD)
    blocks = [data[i:i + PAYLOAD] for i in range(0, len(data), PAYLOAD)]
    out = bytearray()
    for n, chunk in enumerate(blocks):
        header = struct.pack("<8I", UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_FLAG_FAMILY_ID,
                             base + n * PAYLOAD, PAYLOAD, n, len(blocks), family)
        body = chunk + b"\x00" * (476 - PAYLOAD)
        out += header + body + struct.pack("<I", UF2_MAGIC_END)
    return bytes(out)


def main() -> None:
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    boot_path, app_path, app_start, family, prefix = sys.argv[1:]
    app_start = int(app_start, 0)
    family = int(family, 0)

    boot = open(boot_path, "rb").read()
    app = open(app_path, "rb").read()

    if len(boot) > app_start:
        sys.exit(f"bootloader is {len(boot)} bytes, does not fit below {app_start:#x}")
    if not app or app[:4] in (b"\xff\xff\xff\xff", b"\x00\x00\x00\x00"):
        sys.exit("firmware image looks empty")

    with open(prefix + ".uf2", "wb") as f:
        f.write(to_uf2(app, app_start, family))
    with open(prefix + "_full.bin", "wb") as f:
        f.write(boot + b"\xff" * (app_start - len(boot)) + app)

    print(f"bootloader {len(boot)} B, firmware {len(app)} B at {app_start:#x}")
    print(f"wrote {prefix}.uf2 and {prefix}_full.bin")


if __name__ == "__main__":
    main()
