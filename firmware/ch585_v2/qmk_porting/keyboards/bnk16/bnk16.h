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

/* Switch to wired mode: stops BLE and remembers the mode (bnk16.c) */
void enter_wired_mode(void);
