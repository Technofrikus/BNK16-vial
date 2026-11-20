/*
 * Binepad BNK16 board definition: 4x4 keys and 2 encoder clicks on matrix row 4.
 * The layer switch contacts on matrix (0,4) and (1,4) are not keys: they are always KC_NO, are not
 * listed in vial.json, and are read directly by layer_switch_task() in bnk16.c.
 */
#pragma once

#include "quantum.h"
#include "extra_keycode.h"

// clang-format off
#define LAYOUT( \
    K00, K01, K02, K03, \
    K10, K11, K12, K13, \
    K20, K21, K22, K23, \
    K30, K31, K32, K33, \
    K40, K41            \
) \
{ \
    { K00,   K01,   K02,   K03,   KC_NO }, \
    { K10,   K11,   K12,   K13,   KC_NO }, \
    { K20,   K21,   K22,   K23,   KC_NO }, \
    { K30,   K31,   K32,   K33,   KC_NO }, \
    { K40,   K41,   KC_NO, KC_NO, KC_NO }  \
}
// clang-format on

/* True in the USB position of the slide switch (bnk16.c) */
extern bool wired_mode_active;

/* Haptic and big knob settings, used by the custom keycodes in keymap.c */
void bnk16_haptic_reset(void);
void bnk16_haptic_change_freq(bool up);
void bnk16_haptic_change_length(bool up);
void bnk16_knob_change_resolution(bool up);
void bnk16_haptic_toggle_brake(void);
bool bnk16_haptic_brake_on(void);
bool bnk16_keycode_is_haptic_brake(uint16_t keycode);
uint8_t bnk16_knob_steps(void);

/* Light sleep time (the time between the LEDs going off and the deep sleep): SLEEP_TIME_DOWN/UP change it,
 * bnk16_light_sleep_ms() is read by power_management.c. SETTINGS_TYPE types all settings as text */
void bnk16_sleep_time_change(bool longer);
uint32_t bnk16_light_sleep_ms(void);
void bnk16_type_settings(void);

/* The keys of the settings with a range light from green to red as they near the end of it (bnk16.c).
 * bnk16_keycode_setting() is in keymap.c, bnk16_setting_room() says how many steps are left in the direction */
enum { BNK16_SETTING_NONE = 0, BNK16_SETTING_KNOB, BNK16_SETTING_SLEEP, BNK16_SETTING_FREQ, BNK16_SETTING_LENGTH };
uint8_t bnk16_keycode_setting(uint16_t keycode, bool *up);
void bnk16_setting_room(uint8_t setting, bool up, uint8_t *left, uint8_t *range);

/* LED settings and states, used by keymap.c */
void bnk16_toggle_show_unassigned(void);
bool bnk16_show_unassigned(void);
uint8_t bnk16_keycode_bt_device(uint16_t keycode);

/* Battery messages on the status LED: the warning check runs on every key press (keymap.c), BATTERY_SHOW
 * shows the level as a colour; power_management.c runs the empty battery shutdown (red first, then off) */
void bnk16_low_battery_check(void);
void bnk16_battery_show(void);
void bnk16_battery_empty_task(bool empty);
bool bnk16_battery_empty_at_boot(void);

/* Shutdown and deep sleep, used by power_management.c (bnk16_shutdown_possible() also gates the deep sleep) */
bool bnk16_shutdown_possible(void);
__attribute__((noreturn)) void bnk16_shutdown(void);
#ifdef BNK16_DEEP_SLEEP
void bnk16_deep_sleep_enter(void);
bool bnk16_deep_sleep_poll(void);
bool bnk16_deep_sleep_woken(void);
void bnk16_deep_sleep_exit(void);
#endif
