#!/usr/bin/env python3
"""Wrap a sleep test binary as a UF2 for the BNK16 bootloader.

Usage: make_uf2.py APP_BIN APP_START FAMILY_ID OUT_UF2
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bootloader"))
from make_images import to_uf2  # noqa: E402

if len(sys.argv) != 5:
    sys.exit(__doc__)
app_path, app_start, family, out = sys.argv[1:]
app = open(app_path, "rb").read()
if not app or app[:4] in (b"\xff\xff\xff\xff", b"\x00\x00\x00\x00"):
    sys.exit(f"{app_path} looks empty")
with open(out, "wb") as f:
    f.write(to_uf2(app, int(app_start, 0), int(family, 0)))
print(f"{out}: {len(app)} B at {app_start}")
