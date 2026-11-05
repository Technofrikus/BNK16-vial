/*
 * BNK16 Vial keymap: numpad on layer 0, the Bluetooth keys in the top row of layer 3, layers 1-2 and the
 * rest of layer 3 empty (layers are selected by the 4-position switch). Empty keys are KC_NO, so they stay
 * dark until assigned; encoder clicks and rotation stay transparent.
 *
 * Custom keycodes (same order as customKeycodes in vial.json):
 *   BT_1 / BT_2 / BT_3  tap: switch to that device, hold 3 s: pair it; nothing in the USB switch position
 *   BT_CLEAR            clear all bonds at once on a single tap, so not on the default map
 *   FACTORY_RESET       hold 5 s: clear bonds and settings, then reboot
 *   HAPTIC_FREQ_DOWN/UP burst frequency, 8 Hz per press
 *   HAPTIC_LEN_DOWN/UP  burst length, 1 push pulse per press
 *   HAPTIC_BRAKE        brake pulse after the burst on/off
 *   KNOB_RES_DOWN/UP    big knob resolution (key presses and haptic bursts per turn: 80 / steps per key, 1-8)
 *   HAPTIC_RESET        all haptic settings and the knob resolution back to the defaults
 *   LED_MASK            light unassigned keys too / keep them dark (default)
 */

#include QMK_KEYBOARD_H
#include "ble_app.h"
#include "eeconfig.h"
#include "power_management.h"

/* Vial shows and stores its custom keycodes as QK_KB_0 + index, in the order of customKeycodes in vial.json */
enum custom_keycodes {
    BT_1 = QK_KB_0,
    BT_2,
    BT_3,
    BT_CLEAR,
    FACTORY_RESET,
    HAPTIC_FREQ_DOWN,
    HAPTIC_FREQ_UP,
    HAPTIC_LEN_DOWN,
    HAPTIC_LEN_UP,
    HAPTIC_BRAKE,
    KNOB_RES_DOWN,
    KNOB_RES_UP,
    HAPTIC_RESET,
    LED_MASK,
};

// clang-format off
#define EMPTY_LAYER LAYOUT( \
    KC_NO,   KC_NO,   KC_NO,   KC_NO,   \
    KC_NO,   KC_NO,   KC_NO,   KC_NO,   \
    KC_NO,   KC_NO,   KC_NO,   KC_NO,   \
    KC_NO,   KC_NO,   KC_NO,   KC_NO,   \
    KC_TRNS, KC_TRNS                    \
)

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_P7,   KC_P8,   KC_P9,   KC_PSLS,
        KC_P4,   KC_P5,   KC_P6,   KC_PAST,
        KC_P1,   KC_P2,   KC_P3,   KC_PMNS,
        KC_P0,   KC_PDOT, KC_PENT, KC_PPLS,
        KC_MPLY, KC_MFFD
    ),
    [1] = EMPTY_LAYER,
    [2] = EMPTY_LAYER,
    [3] = LAYOUT(
        BT_1,    BT_2,    BT_3,    KC_NO,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_NO,   KC_NO,   KC_NO,   KC_NO,
        KC_TRNS, KC_TRNS
    ),
};

#if defined(ENCODER_MAP_ENABLE)
#define TRANSPARENT_ENCODERS { ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS) }

/* Encoder 0 = small left (display brightness), encoder 1 = small right (Ctrl+/ and Ctrl+], which are
 * zoom out/in on a German layout), encoder 2 = big knob (fine volume steps on macOS) */
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = { ENCODER_CCW_CW(KC_BRID, KC_BRIU), ENCODER_CCW_CW(LCTL(KC_SLSH), LCTL(KC_RBRC)), ENCODER_CCW_CW(LSA(KC_KB_VOLUME_DOWN), LSA(KC_KB_VOLUME_UP)) },
    [1] = TRANSPARENT_ENCODERS,
    [2] = TRANSPARENT_ENCODERS,
    [3] = TRANSPARENT_ENCODERS,
};
#endif
// clang-format on

/* ========================================================================
 * Long press handling for the custom keycodes
 * ======================================================================== */
#define BT_PAIR_HOLD_MS       3000
#define FACTORY_RESET_HOLD_MS 5000

static uint16_t long_press_keycode;   /* key being timed, 0 = none */
static uint32_t long_press_start;
static bool     long_press_triggered; /* fires once per press */

/* BT_1/2/3 to device number 1/2/3 */
static uint8_t device_of(uint16_t keycode) {
    return keycode - BT_1 + 1;
}

static uint32_t hold_time_of(uint16_t keycode) {
    return keycode == FACTORY_RESET ? FACTORY_RESET_HOLD_MS : BT_PAIR_HOLD_MS;
}

/* The EEPROM driver writes synchronously, so the reset can follow at once */
static void factory_reset(void) {
    ble_clear_bonds();
    eeconfig_init();
    PFIC_SystemReset();
}

/* Used by the LED overlay in bnk16.c */
bool bnk16_keycode_is_haptic_brake(uint16_t keycode) {
    return keycode == HAPTIC_BRAKE;
}

bool bnk16_keycode_is_knob_res(uint16_t keycode, bool up) {
    return keycode == (up ? KNOB_RES_UP : KNOB_RES_DOWN);
}

/* Device number 1-3 of a BT key, 0 for any other key */
uint8_t bnk16_keycode_bt_device(uint16_t keycode) {
    return keycode >= BT_1 && keycode <= BT_3 ? device_of(keycode) : 0;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        power_notify_activity();
    }

    /* after a reconnect timeout any key restarts the reconnect; the BT keys choose a slot themselves */
    if (record->event.pressed && (keycode < BT_1 || keycode > BT_3)) {
        ble_wake_from_standby();
    }

    if (keycode == LED_MASK) {
        if (record->event.pressed) {
            bnk16_toggle_show_unassigned();
        }
        return false;
    }

    if (keycode >= HAPTIC_FREQ_DOWN && keycode <= HAPTIC_RESET) {
        if (record->event.pressed) {
            switch (keycode) {
                case HAPTIC_FREQ_DOWN:
                case HAPTIC_FREQ_UP:
                    bnk16_haptic_change_freq(keycode == HAPTIC_FREQ_UP);
                    break;
                case HAPTIC_LEN_DOWN:
                case HAPTIC_LEN_UP:
                    bnk16_haptic_change_length(keycode == HAPTIC_LEN_UP);
                    break;
                case HAPTIC_BRAKE:
                    bnk16_haptic_toggle_brake();
                    break;
                case HAPTIC_RESET:
                    bnk16_haptic_reset();
                    break;
                default:
                    bnk16_knob_change_resolution(keycode == KNOB_RES_UP);
                    break;
            }
        }
        return false;
    }

    if (keycode < BT_1 || keycode > FACTORY_RESET) {
        return true;
    }

    if (record->event.pressed) {
        switch (keycode) {
            case BT_CLEAR:
                ble_clear_bonds();
                break;
            default:
                long_press_keycode   = keycode;
                long_press_start     = timer_read32();
                long_press_triggered = false;
                break;
        }
    } else if (keycode == long_press_keycode) {
        /* released before the hold time: only the BT keys have a tap action */
        if (!long_press_triggered && keycode <= BT_3) {
            ble_switch_device(device_of(keycode));
        }
        long_press_keycode = 0;
    }
    return false;
}

void housekeeping_task_user(void) {
    if (long_press_keycode != 0 && !long_press_triggered && timer_read32() - long_press_start >= hold_time_of(long_press_keycode)) {
        long_press_triggered = true;
        if (long_press_keycode == FACTORY_RESET) {
            factory_reset();
        } else {
            ble_pair_device(device_of(long_press_keycode));
        }
    }

    /* retry a HID report the BLE stack could not take yet */
    ble_retry_pending_report();
}
