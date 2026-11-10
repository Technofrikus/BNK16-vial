# Phase 47 hand-over: deep sleep with the BLE link kept (state 2026-11-10)

Read this first in a new session, then the phase 47 rows (47 to 47q) in the file table of `PROJECT_NOTES.md`, which hold every build, SHA256 and board result in detail.

Repo: `/Users/tf/Nextcloud/Keyboard/BNK16/vial-build/ch585-qmk-vial`, branch `feat/official-ble-deep-sleep`, on top of `980cb67`, committed on 2026-11-10 in two commits (tests, then the deep sleep), not pushed. `README.md` and `pictures/` are the owner's own uncommitted edits. UF2s and their elfs are in `/Users/tf/Nextcloud/Keyboard/BNK16/` and `.../BNK16/elf/`.

## Where it stands

> **Correction, 2026-11-10 (47t):** the link is **not** kept in deep sleep. The chip sleeps and wakes cleanly, but with the chip sleep on the radio does not receive: the link is lost and every reconnect fails with reason 0x3e until a key keeps the chip awake. "Deep sleep with the link kept works" below is wrong, and so are the currents. The waking key is not typed because the link is down at that moment. 47u (`R8_SLP_POWER_CTRL |= 0x40` back in `SetSysClock()`, as the website EVT) did not change it: the first link ends by supervision timeout exactly 5 s after the deep sleep begins. 47v (deep sleep loop thinned to TMOS plus housekeeping every 250 ms, BLE interrupt priority as the EVT) fixed the deep sleeps after the first one: link held for 25 s there, also at 100-120 ms, and the waking key is typed. **Solved in 47y:** in deep sleep the main loop now runs only `TMOS_SystemProcess()` from RAM (`deep_sleep_spin()` in `power_management.c`), our code gets one pass every 250 ms. Link kept through every deep sleep (0 losses, 85 s), waking key typed, 800 uA at 15 ms and 180 uA at 100-120 ms. The cause was our own code running after each chip wake-up, 1.6 ms ahead of the radio event. Rule for later changes: nothing may run per pass in deep sleep. 47z: link holds without the BLE interrupt priority (stays off); test sleep timer now 15 s. Phase 48: knob wake-ups work (the resets in 47z were the test watchdog firing in the send wait of `ble_wait_ms()`). **On the bench, untested: `bnk16-phase48b-clean-test-timers.uf2` and `bnk16-phase48c-clean-daily.uf2`, the same source without diagnostics.** 48b tested: currents, link, USB, Vial and shutdown all good. Still open: link and current over the 25 min of a daily deep sleep (48c). See the 47q-47u rows in `PROJECT_NOTES.md`.

- **Deep sleep with the link kept works.** From 30 s after the last key (test timers) the chip sleeps between radio events with the link up: hundreds of sleeps in a row, every wake-up by the RTC, no reboot, no hang. Current on the meter 0.4-10.5 mA (jumps between sleep and radio events; a real average is not measured yet; test B reference: 0.76 mA at 15 ms, about 150 uA at 100-120 ms).
- **Wake-up works**: by a key (no hang), by the slide switch to USB (wired mode follows, no reboot), by the built-in 20 s timed exit of the diagnostic build. After a wake-up Vial over the cable works. Shutdown at 90 s works (60 uA).
- **Open:** (1) **the key that wakes the board is not typed** (47p); 47q, the current build on the board, types a `wake …` line to find out why; the user brings its result to the new session. (2) Real average current at 100-120 ms (from 60 s). (3) Link held for minutes/an hour in deep sleep. (4) Clean build without the diagnostics (watchdogs, reports, boot colour), first with 30 s timers, then the daily timers (5/10/30 min). (5) Optional: the deep sleep main loop spins a lot between sleeps (about 9800 loop passes against 580 sleeps in 20 s, there is no WFI in deep sleep); check the average current before optimising.

## The bugs found on the way (all fixed in the current source)

1. `RTC_SetTignTime()` (`sdk/HAL/RTC.c`) ran from flash; `CH58X_LowPower()` calls it right after the wake-up, before flash and clock are back: crash on the first sleep. Fix: `__HIGH_CODE`, as in the WCH website EVT. **This was also the phase 38 failure.**
2. The SDK's `HardFault_Handler` resets in a way that reports as a **power-on reset**, so every crash looked like a power loss (white boot colour, keep register wiped). That misled phases 38 and 47-47f.
3. `LowPower_Sleep_WFE()` leaves SLEEPDEEP set; our main loop's `__WFI()` would then deep-sleep unprepared. Fix: `SLEEPDEEP_CLEAR()` after every wake-up in `SLEEP.c`. Also: **no `__WFI()` in `main()` while in deep sleep** (loop runs TMOS back to back as WCH's examples do).
4. `SYS_DisableAllIrq()` masks every interrupt in the PFIC and `SYS_RecoverIrq()` switches the saved ones on again, so `bnk16_deep_sleep_enter()` re-enabled the USB HS interrupt (and TMR2) after switching USB off: USB interrupt storm after the first sleep. Fix: disable IRQ 37 and TMR2 again after the restore.
5. On wake-up the USB HS block came back with a flag its handler cannot clear (storm right after `USBHS_Device_Init()`). Fix: `usbhs_on_clean()`: PLL on, protocol engine/link reset, init, clear all flags and the PFIC pending bit, with interrupts off globally.
6. **The firmware runs in user mode** (WCH startup): `mstatus` is not accessible (illegal instruction). Global interrupt disable for user code = WCH CSR `0x800` with mask `0x88`.
7. `BB_IRQHandler` (`ble/ble_app.c`) was a plain function in flash that jumped to `BB_IRQLibHandler` (returns with `ret`); now a real `__INTERRUPT __HIGH_CODE` handler. It has never fired in any test (counter `b0`).
8. Halt guard in `CH58X_LowPower()`: skip the halt if the real wake-up time is under 4 RTC ticks away or past (never triggered so far, `h0`).

Diagnostic-only lessons: the watchdog enable survives a software reset, so a watchdog that is always on is inherited by the UF2 bootloader and can break a firmware update (47j); `send_string()` of a long line blocks longer than the 0.5 s watchdog (47i); `timer_read32() | 1` as a "set" marker lies 1 ms in the future on even times and `timer_elapsed32()` then is huge (47o/p).

## How the code is organised now

- Switch: `BNK16_DEEP_SLEEP` in `qmk_porting/keyboards/bnk16/mcuconf.h` (sets `HAL_SLEEP 1`); `HAL_SLEEP_XT_PRE` (default 1, website EVT path chosen by `chip_info`).
- `bnk16.c` section "Deep sleep and shutdown": `sleep_pins_arm()` (shared with `bnk16_shutdown()`), `usbhs_off()`, `usbhs_on_clean()`, `bnk16_deep_sleep_enter/poll/woken/exit()`; PB11 is polled every 500 ms in deep sleep instead of being armed.
- `power_management.c`: `hal_sleep_allowed()`, `power_deep_sleep_task()`, `power_in_deep_sleep()`; hook in `platform_run()` (`protocol/protocol_usb.c`, weak default).
- `sdk/HAL/SLEEP.c`: `CH58X_LowPower()` from the WCH website EVT plus the veto, SLEEPDEEP clear, halt guard, breadcrumbs. `sdk/HAL/RTC.c`: `RTC_SetTignTime` in RAM. `sdk/HAL/MCU.c`: no `WakeUpTime`; optional `HAL_BLE_IRQ_PRIORITY`.
- Diagnostics (test builds only, `BNK16_DEFS="-DBNK16_BOOT_DIAG -DBNK16_FAULT_DIAG"`): `bnk16_fault.h` (RAM block at 0x2001C000, stages, counters), own HardFault/NMI handlers, hardware and TMR3 soft watchdog armed only in deep sleep, `fault …`/`stats …`/`wake …` lines typed over BLE, boot colour of the reset cause. Also touched for them: `user/usb/ch585_usbhs_device.c` (counter), `user/usb/usbd_compostie_km.c` (demo `TMR3_IRQHandler` made weak).
- Build switches: `BNK16_TEST_TIMERS=1`, `BNK16_DEFS`, `BNK16_BLE_LIB_DIR` (`build-bnk16.sh`); `BLETEST_EVT=web`, `BLETEST_REGDUMP`, `BLETEST_VARIANTS` (`build-bletest.sh`, test B against the website EVT copy in the git-ignored `tests/bletest/evt-web/`).
- Build: `docker run --rm --platform linux/amd64 -e BNK16_TEST_TIMERS=1 -e BNK16_DEFS="-DBNK16_BOOT_DIAG -DBNK16_FAULT_DIAG" -v "$PWD:/workspace" nookcraftlab/riscv:latest bash /workspace/firmware/ch585_v2/build-bnk16.sh` from the repo root (start Docker Desktop first if needed). Decode addresses with `riscv-wch-elf-addr2line` from the image against the kept elf.

## Reading the 47q lines

- `stats c n w k l p m h s`: sleep calls allowed, sleeps, wake-ups, too short, too late, non-RTC wake-ups, deep sleep loop passes, halts skipped, last stage (hex).
- `wake cC tT sS kK pP dD`: C column pins PB4-PB7 at the exit (0 bit = pressed), T RTC ticks (31 us) pin interrupt to exit, S QMK scans before the first key event, K its keycode (ffff = none in 3 s), P 1 press/0 release, D ms after the exit. The line after the timed exit has no key (T meaningless).
- `fault F …`: F 1 exception, 3 soft watchdog; `e` = address to decode; `r` reset flag of the boot.
- Boot colour (first 4 s): white power-on, yellow software reset (our fault handlers), cyan watchdog, green woken from shutdown, red power-on during sleep.

## Flashing notes

Flash after a power cycle (switch off/on, hold the top-left key at power-on for BNK16BOOT). Do not use the diagnostic builds with Vial writes (watchdog in deep sleep). Daily firmware to go back to: `bnk16-phase46b-web-ble.uf2`.
