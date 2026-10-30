# Upstreaming plan

## Where changes can go

| Project | Fit |
|---|---|
| [NookCraftLab/ch585-qmk-vial](https://github.com/NookCraftLab/ch585-qmk-vial) | **The target.** All our port changes are in files of this repository |
| [NookCraftLab/qmk_firmware](https://github.com/NookCraftLab/qmk_firmware) | Nothing to send: we changed nothing in the submodule |
| vial-kb/vial-qmk, qmk/qmk_firmware | Not a fit for the port: they have no CH58x platform, and the wireless part depends on WCH's closed library |

## Candidate pull requests

GitHub only opens PRs from a real fork, and BNK16-vial is a standalone repo. PR branches therefore go to the fork [Technofrikus/ch585-qmk-vial](https://github.com/Technofrikus/ch585-qmk-vial).

| Remote | Repository | Use |
|---|---|---|
| `origin` | Technofrikus/BNK16-vial | Public BNK16 firmware, branch `main` |
| `fork` | Technofrikus/ch585-qmk-vial | Topic branches for PRs only |
| `upstream` | NookCraftLab/ch585-qmk-vial | PR target, never pushed to directly |

To set this up in a fresh clone:

```bash
git remote add fork https://github.com/Technofrikus/ch585-qmk-vial.git
```

```bash
git remote add upstream https://github.com/NookCraftLab/ch585-qmk-vial.git
```

Each PR is against `upstream/main`, independent of the others unless noted. Never push BNK16-vial's `main` to the fork, because a PR must only contain its own topic. To build a branch and open the PR:

```bash
git fetch upstream
```

```bash
git switch -c <topic> upstream/main
```

```bash
git restore --source=main -- <files>
```

```bash
git commit -m "fix: <summary>"
```

```bash
git push -u fork <topic>
```

```bash
gh pr create --repo NookCraftLab/ch585-qmk-vial --base main --head Technofrikus:<topic>
```

Build the EVB target on the branch before opening the PR, because `bnk16` does not exist on `upstream/main`. Switch back with `git switch main`.

| # | Topic | Files | Before sending |
|---|---|---|---|
| 1 | WS2812 PWM driver: current QMK API, single-shot DMA, clean frame start | `qmk_porting/drivers/ws2812/ws2812_pwm.c`, `drivers/led` include in `common_features.cmake` | The TMR2 branch is untested. `WS2812_DI_HIGH_DRIVE` assumes port A |
| 2 | USB: 1 ms keyboard polling, wait for a free endpoint, working media keys and mouse | `qmk_porting/protocol/usb_interface.c`, `user/usb/usb_desc.c`, `usb_desc.h`, `ch585_usbfs_device.c`, `ch585_usbfs_device.h` | Only the HS path is tested. The BNK16 has no FS connector, so test FS on the EVB |
| 3 | Per-board USB port selection | `user/usb/usb_config.h` | Default stays dual port, EVB build checked |
| 4 | VialRGB and missing RGB source in the build | `qmk_porting/common_features.cmake` (rest of the file) | |
| 5 | Report the real USB state to QMK | New, currently a workaround in `bnk16.c` (`usb_is_connected`) | The port never calls `usb_device_state_set_configuration()`, so its suspend handling never runs. Fix it in the port, then drop the workaround |
| 6 | BNK16 board target | `qmk_porting/keyboards/bnk16/`, `build-bnk16.sh` | See below. Depends on 1-4 |

## Blockers for the board target

- Remove `VIAL_INSECURE`.
- Replace the pid.codes test ID `1209:0001` with an assigned VID/PID.
- Bring up the mode switch or remove the BLE keycodes from `vial.json`.

## Conventions upstream asks for

Its README asks for Chinese comments that describe function only, without history, and commit prefixes `feat:`, `fix:`, `docs:`, `refactor:`, `chore:`. Our new comments are in English. Ask the maintainer whether English is acceptable before translating.

The big knob's direction hysteresis (`bnk16.c`) would only be useful more widely as a generic option in QMK's encoder driver. It stays board code for now.
