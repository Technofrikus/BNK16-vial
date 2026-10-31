/*
 * Binepad BNK16 board config.
 * Matrix, encoder, LED and layer switch pins are measured on the board, see docs/bnk16/HARDWARE.md.
 * VBUS, charger and battery pins are unknown and unused here.
 */
#pragma once

/* USB Device descriptor parameter. Self-chosen, not registered with the USB-IF: private use only.
 * VID = "VP" in ASCII. Vial shows the manufacturer and product strings, see the _UTF16 lists. */
#define VENDOR_ID    0x5650
#define PRODUCT_ID   0x0B16
#define DEVICE_VER   0x0001
#define MANUFACTURER Vialpad
#define PRODUCT      BNK16
#define MANUFACTURER_UTF16 'V', 0, 'i', 0, 'a', 0, 'l', 0, 'p', 0, 'a', 0, 'd', 0
#define PRODUCT_UTF16      'B', 0, 'N', 0, 'K', 0, '1', 0, '6', 0

/* Matrix (measured on the board): rows PB3 PB2 PB0 PB1 PB21 (row 4 = encoder clicks), cols PB5 PB4 PB6 PB7.
 * Col 4 = PB10 is the common line of the 4-position layer switch (traced on the PCB): its two other
 * lines go through diodes to rows 0 (PB3) and 1 (PB2), so the switch position reads as matrix (0,4)/(1,4).
 * PB10 is only free because USB runs on the HS port alone (usb_config.h). */
#define MATRIX_ROWS 5
#define MATRIX_COLS 5
#define MATRIX_ROW_PINS { B3, B2, B0, B1, B21 }
#define MATRIX_COL_PINS { B5, B4, B6, B7, B10 }
#define DIODE_DIRECTION  COL2ROW
#define HOLD_ON_OTHER_KEY_PRESS
#define EARLY_INIT_PERFORM_BOOTLOADER_JUMP FALSE
#define DEBOUNCE 10

/* Encoders (measured): index 0 = small left, index 1 = small right, index 2 = big knob (no switch).
 * Small clicks are matrix (4,0) and (4,1). PB14/PB15 double as the TIO/TCK debug pins, PB16+ have
 * their own digital-input-disable bits; bnk16.c makes all of them plain GPIO inputs before encoder init. */
#define ENCODER_A_PINS { B14, B17, B8 }
#define ENCODER_B_PINS { B15, B16, B9 }
/* Resolution = pin edges per output step. Small knobs: 20 detents, one full quadrature cycle (4 edges)
 * per detent -> 4 (use 2 if a step only comes every second detent). Big knob: no detents, 20 cycles
 * per turn; 1 counts every intermediate state too (80 steps per turn). */
#define ENCODER_RESOLUTIONS { 4, 4, 1 }
#define BNK16_BIG_ENCODER 2
/* Big knob steps per key press, default 2 = 40 key presses per turn. Custom keycodes in Vial change it,
 * saved in EEPROM. Every key press gives one haptic burst, steps that send no key give none. */
#define BNK16_BIG_STEPS_PER_KEY 2
#define BNK16_BIG_STEPS_PER_KEY_MAX 8
/* Big knob keys come at least this far apart, faster steps are dropped: hosts can be slower than the
 * knob (macOS volume keys) and would keep working through the backlog after the knob stopped. */
#define BNK16_BIG_MIN_KEY_INTERVAL_MS 19
/* Key press and release of an encoder map step are this far apart. The default (TAP_CODE_DELAY, 30 ms)
 * blocked the main loop for 60 ms per step and capped the knob at about 16 steps/s. */
#define ENCODER_MAP_KEY_DELAY 2
/* Ring buffer, holds one event less than this */
#define MAX_QUEUED_ENCODER_EVENTS 4

/* RGB: WS2812 on PA10 (TMR1 PWM + DMA). Chain = 16 key LEDs (row-major) + 1 status LED. */
#define WS2812_DI_PIN A10
#define WS2812_DI_HIGH_DRIVE
/* LED supply is switched by PA11, high = on (measured) */
#define WS2812_EN_PIN A11
#define WS2812_EN_LEVEL 1
#define WS2812_PWM_TARGET_PERIOD 800000 /* 800 kHz bit clock (driver default is 10x too slow) */
#define RGB_MATRIX_LED_COUNT 17
#define BNK16_STATUS_LED 16             /* chain index of the extra status LED, excluded from effects */
#define RGB_MATRIX_MAXIMUM_BRIGHTNESS 128
#define RGB_MATRIX_DEFAULT_VAL 96
#define RGB_MATRIX_DEFAULT_MODE RGB_MATRIX_CYCLE_LEFT_RIGHT
#define RGB_MATRIX_KEYPRESSES
#define ENABLE_RGB_MATRIX_SOLID_COLOR
#define ENABLE_RGB_MATRIX_BREATHING
#define ENABLE_RGB_MATRIX_BAND_SAT
#define ENABLE_RGB_MATRIX_CYCLE_ALL
#define ENABLE_RGB_MATRIX_CYCLE_LEFT_RIGHT
#define ENABLE_RGB_MATRIX_CYCLE_UP_DOWN
#define ENABLE_RGB_MATRIX_CYCLE_PINWHEEL
#define ENABLE_RGB_MATRIX_RAINBOW_MOVING_CHEVRON
/* Reactive effects (react to key presses, need RGB_MATRIX_KEYPRESSES above) */
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_SIMPLE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_WIDE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTIWIDE
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_CROSS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTICROSS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_NEXUS
#define ENABLE_RGB_MATRIX_SOLID_REACTIVE_MULTINEXUS
#define ENABLE_RGB_MATRIX_SPLASH
#define ENABLE_RGB_MATRIX_MULTISPLASH
#define ENABLE_RGB_MATRIX_SOLID_SPLASH
#define ENABLE_RGB_MATRIX_SOLID_MULTISPLASH

/* Haptic motor on PA7 (measured), low = on (P-FET). It only vibrates on AC, so it gets a square wave.
 * Every big knob key press gives one burst, to simulate detents. Haptics are switched off with the
 * hardware switch only: the firmware cannot see it.
 * These are the defaults: custom keycodes in Vial change frequency, length and the brake pulse,
 * or reset all of them; they are saved in EEPROM. */
#define BNK16_HAPTIC_FREQ_HZ 128
#define BNK16_HAPTIC_FREQ_MIN 64
#define BNK16_HAPTIC_FREQ_MAX 256
#define BNK16_HAPTIC_FREQ_STEP 8
#define BNK16_HAPTIC_PULSES 2      /* push pulses per burst, 1 = a single push of half a period */
#define BNK16_HAPTIC_PULSES_MAX 16
#define BNK16_HAPTIC_BRAKE 1       /* brake pulse after the burst */

/* Vial */
#define VIAL_KEYBOARD_UID {0xDE, 0x8B, 0x27, 0x0F, 0x95, 0xED, 0x79, 0xBB}
/* Always unlocked for now; removing this activates the unlock combo below */
#define VIAL_INSECURE
#define VIAL_UNLOCK_COMBO_ROWS {0, 0}
#define VIAL_UNLOCK_COMBO_COLS {0, 3}
#define DYNAMIC_KEYMAP_LAYER_COUNT 4

/* Vial's advanced features: vial.h needs these to enable its VIAL_xxx_ENABLE parts */
#define TAP_DANCE_ENABLE
#define VIAL_TAP_DANCE_ENABLE
#define COMBO_ENABLE
#define VIAL_COMBO_ENABLE
#define KEY_OVERRIDE_ENABLE
#define VIAL_KEY_OVERRIDE_ENABLE
#define REPEAT_KEY_ENABLE
#define VIAL_ALT_REPEAT_KEY_ENABLE

#define VIAL_TAP_DANCE_COUNT 16
#define TAPPING_TERM 200
#define TAPPING_TERM_PER_KEY
#define VIAL_COMBO_COUNT 16
#define COMBO_TERM 50
#define VIAL_KEY_OVERRIDE_COUNT 16

/* QMK features switched on in keymaps/vial/rules.cmake */
#define ONESHOT_TAP_TOGGLE 5            /* taps that lock a one-shot modifier */
#define ONESHOT_TIMEOUT 5000            /* ms */
#define BOTH_SHIFTS_TURNS_ON_CAPS_WORD
#define CAPS_WORD_IDLE_TIMEOUT 5000     /* ms */

/* the USB port code overwrites the endpoint buffer, so press and release need a gap */
#define TAP_CODE_DELAY 30

/* Diagnostics (bnk16_diag.c), they type their results over USB:
 * pin scan drives each free pin in turn, switch probe reports every pin state change. */
// #define BNK16_PIN_SCAN
// #define BNK16_SWITCH_PROBE
