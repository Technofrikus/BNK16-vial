/*
 * Binepad BNK16 board definition: 4x4 keys, 2 encoder clicks on matrix row 4,
 * layer switch contacts on matrix (0,4) and (1,4)
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
    K40, K41,           \
    K04, K14            \
) \
{ \
    { K00,   K01,   K02,   K03,   K04   }, \
    { K10,   K11,   K12,   K13,   K14   }, \
    { K20,   K21,   K22,   K23,   KC_NO }, \
    { K30,   K31,   K32,   K33,   KC_NO }, \
    { K40,   K41,   KC_NO, KC_NO, KC_NO }  \
}
