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
bool bnk16_keycode_is_knob_res(uint16_t keycode, bool up);

/* LED settings and states, used by keymap.c */
void bnk16_toggle_show_unassigned(void);
bool bnk16_show_unassigned(void);
void bnk16_status_mobile_mode(bool on);
uint8_t bnk16_keycode_bt_device(uint16_t keycode);
