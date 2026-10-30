/*
 * BNK16 default keymap: numpad on layer 0, layers 1-3 transparent (selected by the 4-position switch).
 *
 * Custom keycodes (same order as customKeycodes in vial.json):
 *   BT_1 / BT_2 / BT_3  tap: switch to that device, hold 3 s: pair it
 *   BT_CLEAR            clear all bonds
 *   WIRED_MODE          switch to wired mode
 *   FACTORY_RESET       hold 5 s: clear bonds and settings, then reboot
 *   MOBILE_MODE         hold 3 s: toggle mobile mode (keys locked, 2 s BLE connection interval)
 */

#include QMK_KEYBOARD_H
#include "ble_app.h"
#include "eeconfig.h"
#include "power_management.h"

enum custom_keycodes {
    BT_1 = QK_USER_0,
    BT_2,
    BT_3,
    BT_CLEAR,
    WIRED_MODE,
    FACTORY_RESET,
    MOBILE_MODE,
};

// clang-format off
#define TRANSPARENT_LAYER LAYOUT( \
    KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, \
    KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, \
    KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, \
    KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, \
    KC_TRNS, KC_TRNS                    \
)

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        KC_P7,   KC_P8,   KC_P9,   KC_PSLS,
        KC_P4,   KC_P5,   KC_P6,   KC_PAST,
        KC_P1,   KC_P2,   KC_P3,   KC_PMNS,
        KC_P0,   KC_PDOT, KC_PENT, KC_PPLS,
        KC_MUTE, KC_MPLY
    ),
    [1] = TRANSPARENT_LAYER,
    [2] = TRANSPARENT_LAYER,
    [3] = TRANSPARENT_LAYER,
};

#if defined(ENCODER_MAP_ENABLE)
#define TRANSPARENT_ENCODERS { ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS), ENCODER_CCW_CW(KC_TRNS, KC_TRNS) }

/* Encoder 0 = small left, encoder 1 = small right, encoder 2 = big knob */
const uint16_t PROGMEM encoder_map[][NUM_ENCODERS][NUM_DIRECTIONS] = {
    [0] = { ENCODER_CCW_CW(KC_VOLD, KC_VOLU), ENCODER_CCW_CW(KC_PGUP, KC_PGDN), ENCODER_CCW_CW(KC_WH_U, KC_WH_D) },
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
#define MOBILE_MODE_HOLD_MS   3000
#define FACTORY_RESET_HOLD_MS 5000
#define MOBILE_CONN_INTERVAL  1600 /* 2000 ms in units of 1.25 ms */

static uint16_t long_press_keycode;   /* key being timed, 0 = none */
static uint32_t long_press_start;
static bool     long_press_triggered; /* fires once per press */
static bool     mobile_mode_active;

/* BT_1/2/3 to device number 1/2/3 */
static uint8_t device_of(uint16_t keycode) {
    return keycode - BT_1 + 1;
}

static uint32_t hold_time_of(uint16_t keycode) {
    switch (keycode) {
        case FACTORY_RESET:
            return FACTORY_RESET_HOLD_MS;
        case MOBILE_MODE:
            return MOBILE_MODE_HOLD_MS;
        default:
            return BT_PAIR_HOLD_MS;
    }
}

static void toggle_mobile_mode(void) {
    mobile_mode_active = !mobile_mode_active;
    power_set_mobile_mode(mobile_mode_active);
    if (mobile_mode_active) {
        ble_set_conn_interval_raw(MOBILE_CONN_INTERVAL);
    } else {
        ble_request_conn_interval_force(0);
    }
}

static void factory_reset(void) {
    ble_clear_bonds();
    eeconfig_init();
    /* give the EEPROM writes time to finish before the reset */
    for (volatile int i = 0; i < 1000000; i++) {
    }
    PFIC_SystemReset();
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        power_notify_activity();
    }

    /* Vial stores its custom keycodes as 0x7E00 + index */
    if ((keycode >> 8) == 0x7E && (keycode & 0xFF) <= MOBILE_MODE - BT_1) {
        keycode = BT_1 + (keycode & 0xFF);
    }

    /* mobile mode locks the keyboard, only its own key still works */
    if (mobile_mode_active && keycode != MOBILE_MODE) {
        return false;
    }

    if (keycode < BT_1 || keycode > MOBILE_MODE) {
        return true;
    }

    if (record->event.pressed) {
        switch (keycode) {
            case BT_CLEAR:
                ble_clear_bonds();
                break;
            case WIRED_MODE:
                enter_wired_mode();
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
        switch (long_press_keycode) {
            case FACTORY_RESET:
                factory_reset();
                break;
            case MOBILE_MODE:
                toggle_mobile_mode();
                break;
            default:
                ble_pair_device(device_of(long_press_keycode));
                break;
        }
    }

    /* retry a HID report the BLE stack could not take yet */
    ble_retry_pending_report();
}
