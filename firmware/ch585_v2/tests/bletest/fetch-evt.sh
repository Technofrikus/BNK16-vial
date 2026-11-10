#!/bin/bash
# Fetch WCH's official CH585 EVT (github.com/openwch/ch585) into tests/bletest/evt (git-ignored).
# Test B builds WCH's HID_Keyboard example from it, with the official BLE library, unchanged except
# for the two files kept here (main.c, hidkbd.c). Pinned to the commit checked on 2026-11-09.
set -e
REPO=https://github.com/openwch/ch585
COMMIT=db889195f07b8f3f4e7bf2c55f96a35daa14bebf
LIB_BLOB=8126c81886f6229227261fae564f696733e0402f # EVT v1.6 libCH58xBLE.a, see PROJECT_NOTES.md
DIR="$(cd "$(dirname "$0")" && pwd)/evt"

if [ ! -d "$DIR/.git" ]; then
    git clone --filter=blob:none --no-checkout "$REPO" "$DIR"
fi
git -C "$DIR" fetch --quiet origin "$COMMIT" 2>/dev/null || true
git -C "$DIR" checkout --quiet "$COMMIT"
blob=$(git -C "$DIR" hash-object EVT/EXAM/BLE/LIB/libCH58xBLE.a)
if [ "$blob" != "$LIB_BLOB" ]; then
    echo "unexpected libCH58xBLE.a ($blob)" >&2
    exit 1
fi
echo "EVT at $(git -C "$DIR" log -1 --format='%h %cd') in $DIR"
