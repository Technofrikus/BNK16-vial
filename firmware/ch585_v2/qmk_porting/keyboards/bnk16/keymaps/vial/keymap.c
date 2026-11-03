/*
 * BNK16 Vial keymap: numpad on layer 0, the Bluetooth keys in the top row of layer 3, layers 1-2 and the
 * rest of layer 3 empty (layers are selected by the 4-position switch). Empty keys are KC_NO, so they stay
 * dark until assigned; encoder clicks and rotation stay transparent.
 *
 * Custom keycodes (same order as customKeycodes in vial.json):
 *   BT_1 / BT_2 / BT_3  tap: switch to that device, hold 3 s: pair it; nothing in the USB switch position
 *   BT_CLEAR            clear all bonds at once on a single tap, so not on the default map
 *   FACTORY_RESET       hold 5 s: clear bonds and settings, then reboot
 *   MOBILE_MODE         hold 3 s: toggle mobile mode (keys locked, 2 s BLE connection interval); in the USB
 *                       switch position it can only be turned off
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
#include "bnk16_diag.h"

enum custom_keycodes {
    BT_1 = QK_USER_0,
    BT_2,
    BT_3,
    BT_CLEAR,
    FACTORY_RESET,
    MOBILE_MODE,
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

/* Vial shows and stores its custom keycodes as 0x7E00 + index (QK_KB_0), so use that in the default map too */
#define VIAL_KC(kc) (QK_KB_0 + ((kc) - BT_1))

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
        VIAL_KC(BT_1), VIAL_KC(BT_2), VIAL_KC(BT_3), KC_NO,
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
    bnk16_status_mobile_mode(mobile_mode_active);
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

/* True for the brake key as stored in the keymap (Vial stores its custom keycodes as 0x7E00 + index) */
bool bnk16_keycode_is_haptic_brake(uint16_t keycode) {
    return keycode == HAPTIC_BRAKE || keycode == 0x7E00 + (HAPTIC_BRAKE - BT_1);
}

/* Same for the knob resolution keys */
bool bnk16_keycode_is_knob_res(uint16_t keycode, bool up) {
    uint16_t knob_key = up ? KNOB_RES_UP : KNOB_RES_DOWN;
    return keycode == knob_key || keycode == 0x7E00 + (knob_key - BT_1);
}

/* Device number 1-3 of a BT key, 0 for any other key */
uint8_t bnk16_keycode_bt_device(uint16_t keycode) {
    if ((keycode >> 8) == 0x7E && (keycode & 0xFF) <= BT_3 - BT_1) {
        return (keycode & 0xFF) + 1;
    }
    return keycode >= BT_1 && keycode <= BT_3 ? device_of(keycode) : 0;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
#ifdef BNK16_INTERVAL_DEBUG
        bnk16_diag_note_key();
#endif
        power_notify_activity();
    }

    /* Vial stores its custom keycodes as 0x7E00 + index */
    if ((keycode >> 8) == 0x7E && (keycode & 0xFF) <= LED_MASK - BT_1) {
        keycode = BT_1 + (keycode & 0xFF);
    }

    /* mobile mode locks the keyboard, only its own key still works */
    if (mobile_mode_active && keycode != MOBILE_MODE) {
        return false;
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

    if (keycode < BT_1 || keycode > MOBILE_MODE) {
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
        switch (long_press_keycode) {
            case FACTORY_RESET:
                factory_reset();
                break;
            case MOBILE_MODE:
                if (!wired_mode_active || mobile_mode_active) {
                    toggle_mobile_mode();
                }
                break;
            default:
                ble_pair_device(device_of(long_press_keycode));
                break;
        }
    }

    /* retry a HID report the BLE stack could not take yet */
    ble_retry_pending_report();
}
