# BNK16 hardware notes

Only trust pins marked as measured. The pin tables decoded from the stock firmware turned out to be wrong for the matrix and the encoders.

## Chip

CH585 (`0x8516`), 448 KB code flash, 32 KB data EEPROM. The ROM ISP bootloader cannot be overwritten. `DOWNLOAD_CFG` is PB22: hold BOOT while plugging in USB, the board then enumerates as `1a86:55e0`.

The USB connector is on the high-speed port (PB12/PB13). The full-speed port must stay off, because it would claim PB10/PB11.

## Measured pins

| Function | Pins | Notes |
|---|---|---|
| Matrix rows 0-3 | PB3, PB2, PB0, PB1 | |
| Matrix row 4 | PB21 | Encoder clicks. The RF antenna switch output must stay off, the BLE stack can enable it |
| Matrix cols 0-3 | PB5, PB4, PB6, PB7 | `COL2ROW` |
| Matrix col 4 | PB10 | Common line of the layer switch |
| Layer switch | Matrix (0,4), (1,4) | Via diodes to rows 0 and 1. None = layer 0, (0,4) = 1, (1,4) = 2, both = 3 |
| Small left encoder | A = PB14, B = PB15 | 20 detents, resolution 4. Click at (4,0). The pins double as TIO/TCK, so SWD is disabled at boot |
| Small right encoder | A = PB17, B = PB16 | 20 detents, resolution 4. Click at (4,1) |
| Big knob | A = PB8, B = PB9 | No detents, no switch. 20 quadrature cycles per turn, resolution 1, direction hysteresis in `bnk16.c` |
| WS2812 data | PA10 | TMR1 PWM + DMA. 16 key LEDs (row-major), then 1 status LED (index 16) |
| LED supply enable | PA11 | High = on |
| USB/BT slide switch | PB11 | Low = BT position. The switch also cuts battery +. Pull-up is set, the level is not evaluated yet |

## Unknown

| Function | State |
|---|---|
| Battery ADC | Stock code suggests AIN12, not checked. The enable pin it suggests (PB17) is an encoder pin, so that reading is wrong |
| VBUS detect | No pin known. USB state is taken from the USBHS driver, so an unplugged cable and a suspended host look the same |
| Charger status | No pin known |
| Haptic motor drive | No pin known. The on/off switch is hardware only (GND to the drain of a 3401 P-MOSFET) |
| Debug UART | PA9/PA8 from the stock code, unverified |

## Going back to stock

**Untested.** The stock images are not in this repository, because the EEPROM dump contains BLE bonding keys.

1. Enter ISP mode and flash the restore image, a stub that jumps to the unmodified stock application: `BNK16-stock-restore-stub.bin`.
2. Erase or restore the EEPROM.

The official firmware is published in [binepad/binepad-updates](https://github.com/binepad/binepad-updates). The stub has no UF2 bootloader, so later updates still need `wchisp`.
