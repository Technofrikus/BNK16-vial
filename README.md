# BNK16-vial

QMK/Vial firmware for the **Binepad BNK16** macropad (WCH CH585, 16 keys, 3 encoders, 4-position layer switch). It replaces the stock VIA firmware, so Tap Dance, Combos, Key Overrides and macros can be configured in [Vial](https://get.vial.today/).

Based on [NookCraftLab/ch585-qmk-vial](https://github.com/NookCraftLab/ch585-qmk-vial). The original README is in [docs/README.upstream.zh.md](docs/README.upstream.zh.md).

## Status

| Feature | State |
|---|---|
| 16 keys, encoder rotation and clicks | works |
| Media keys, mouse wheel, modifier combos such as `LSA(kc)` | works |
| RGB with reactive effects, Vial Lighting tab | works |
| Tap Dance, Combos, macros, settings kept after replug | works |
| Status LED | works: green = USB, blue = BLE, orange = neither |
| 4-position layer switch | works |
| Bluetooth, battery, mode switch | not brought up |
| Haptics | dropped on purpose |
| `VIAL_INSECURE` | still on: Vial is always unlocked, no unlock combo needed |

## Requirements

- [Docker Desktop](https://www.docker.com/products/docker-desktop/), running. Docker holds the whole toolchain, so nothing else needs installing to build. The image is about 8 GB and is downloaded on the first build.
- [wchisp](https://github.com/ch32-rs/wchisp) to flash.
- Git.

## Build

Clone with the QMK submodule:

```bash
git clone --recursive https://github.com/Technofrikus/BNK16-vial.git
```

```bash
cd BNK16-vial
```

If you already cloned without `--recursive`:

```bash
git submodule update --init
```

Build from the repository root:

```bash
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh
```

The result is `firmware/ch585_v2/build-bnk16/bnk16.bin` (also `.hex` and `.elf`). The script deletes that folder first, so every build is a full one.

Why Docker: the build needs WCH's own GCC 12 (`riscv-wch-elf-gcc`), which is Linux x64 only. The repo folder is mounted into the container at `/workspace`, so the output lands in your checkout. `--platform linux/amd64` is needed on Apple Silicon and harmless elsewhere. On Windows PowerShell write `${PWD}` instead of `$PWD`.

## Flash

**Export your layout first** (Vial: File → Save current layout). Every new build resets the stored keymap to the defaults.

1. Hold the BOOT key (PB22) while plugging in USB. The board enumerates as `1a86:55e0`.
2. Flash right away, because ISP mode times out quickly:

```bash
wchisp flash firmware/ch585_v2/build-bnk16/bnk16.bin
```

The bootloader is in mask ROM, so a bad flash cannot brick the board: this procedure always works. Do not run `wchisp config` without a reason, it can trigger a full erase.

Flashing removes Binepad's stock UF2 bootloader, so signed Binepad updates no longer work. A way back to stock exists but is untested, see [docs/bnk16/HARDWARE.md](docs/bnk16/HARDWARE.md#going-back-to-stock).

## Use

Plug the board in and open the Vial app or <https://vial.rocks>. It is detected as `1209:0001` ("BNK16"). On Linux you need the [udev rule from the Vial docs](https://get.vial.today/manual/linux-udev.html).

Default keymap:

| Control | Default |
|---|---|
| Keys | Numpad on layer 0; layers 1-3 are transparent |
| Small left encoder | Volume, click = Mute |
| Small right encoder | Page up/down, click = Play/Pause |
| Big knob | Mouse wheel (no click switch) |
| 4-position switch | Selects layer 0-3 |

In Vial the two extra keys below the layout are the layer switch contacts. Leave them on `KC_NO`.

## Change the firmware

Everything specific to this board is in [firmware/ch585_v2/qmk_porting/keyboards/bnk16/](firmware/ch585_v2/qmk_porting/keyboards/bnk16/):

| File | Content |
|---|---|
| `qmk_config.h` | Pins, encoders, RGB, Vial options |
| `rules.cmake`, `keymaps/default/rules.cmake` | Feature switches. The keymap file is applied last and wins |
| `bnk16.c` | Layer switch, big knob filter, status LED, USB/BLE mode handling, `main()` |
| `keymaps/default/keymap.c` | Default keymap, custom keycodes |
| `vial.json` | Layout shown in Vial |
| `bnk16_diag.c` | Diagnostic builds, see below |

### Diagnostic builds

There is no automated test suite: the firmware is tested on the board. To find pins that are still unknown (battery, VBUS, haptics), uncomment one of these in `qmk_config.h` and rebuild:

| Define | What it does |
|---|---|
| `BNK16_SWITCH_PROBE` | From 5 s after boot, types one line per pin state change |
| `BNK16_PIN_SCAN` | From 20 s after boot, drives each free pin high then low and types its name |

Both type over USB by themselves, so focus an empty text editor first, and do not use them for normal work. Older probes that have served their purpose (click and encoder debugging) are in the Git history, in commit `36851df`.

## Versions

| Part | Version |
|---|---|
| Vial | Identical in `quantum/`, `tmk_core/` and `drivers/` to [vial-kb/vial-qmk](https://github.com/vial-kb/vial-qmk) at `dd43959a` (2026-07-27), the newest commit when checked on 2026-10-30. Vial protocol 6 |
| QMK | The base of that vial-qmk: last merged from QMK on 2025-06-21 (breaking-changes release `20250525`) |
| Submodule | [NookCraftLab/qmk_firmware](https://github.com/NookCraftLab/qmk_firmware) `49a7a153`, a copy of vial-qmk without Git history |
| WCH BLE library | `CH585_BLE_LIB_V1.5.0729`, closed source |
| Toolchain | WCH GCC 12.2 in `nookcraftlab/riscv:latest` |

## Repository layout and upstream

```
firmware/ch585_v2/
  build-bnk16.sh                  build entry point
  qmk_firmware/                   submodule: QMK with Vial, unmodified
  qmk_porting/keyboards/bnk16/    board target
  qmk_porting/drivers/ws2812/     WS2812 driver (changed)
  qmk_porting/protocol/           USB report sending (changed)
  user/usb/                       USB descriptors and driver (changed)
docs/bnk16/                       pin map, upstreaming plan
```

The `upstream` remote is NookCraftLab's repository. Which change could go back there is listed in [docs/bnk16/UPSTREAMING.md](docs/bnk16/UPSTREAMING.md).

## License

GPLv3, see [LICENSE](LICENSE). `libCH58xBLE.a` is WCH's closed-source library, see [docs/LICENSE_NOTICE.md](docs/LICENSE_NOTICE.md).
