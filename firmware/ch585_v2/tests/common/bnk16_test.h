/*
 * Board helpers shared by the BNK16 test firmwares (tests/sleeptest, tests/bletest).
 * Only needs the WCH StdPeriphDriver (CH58x_common.h).
 */
#pragma once

#include "CH58x_common.h"

#define BUZZ_SHORT 3
#define BUZZ_LONG  25

/* Haptic: P-FET gate on PA7, low = motor on. It only moves on AC: 128 Hz square wave, push on every
 * second half period (as in bnk16.c). Busy wait, needs the full clock. */
static inline void buzz(uint8_t pushes) {
    for (uint8_t i = 0; i < 2 * pushes - 1; i++) {
        if (i & 1) {
            GPIOA_SetBits(GPIO_Pin_7);
        } else {
            GPIOA_ResetBits(GPIO_Pin_7);
        }
        mDelayuS(3906);
    }
    GPIOA_SetBits(GPIO_Pin_7);
}

/* Motor off, LEDs unpowered (supply PA11 high = on, data PA10) */
static inline void pins_board_init(void) {
    GPIOA_SetBits(GPIO_Pin_7);
    GPIOA_ModeCfg(GPIO_Pin_7, GPIO_ModeOut_PP_5mA);
    GPIOA_ResetBits(GPIO_Pin_10 | GPIO_Pin_11);
    GPIOA_ModeCfg(GPIO_Pin_10 | GPIO_Pin_11, GPIO_ModeOut_PP_5mA);
}

/* Same states as bnk16_shutdown() (bnk16.c) and the firmware setup before it, without the wake-up arming,
 * so a sleep current compares with the shutdown (test A: 70 uA sleep, 63 uA shutdown):
 *   rows PB3/PB2/PB0/PB1/PB21 driven low, columns PB5/PB4/PB6/PB7 pull-up,
 *   PB10 (layer switch) and PB11 (BT/USB switch) floating, FS USB off PB10/PB11,
 *   left and big knob PB14/PB15/PB8/PB9: pull-up, or pull-down where the contact rests closed,
 *   small right knob PB16/PB17 pull-up (QMK's setting, the shutdown leaves it),
 *   PA6 (battery divider) floating. */
static inline void pins_measure_init(void) {
    const uint32_t rows = GPIO_Pin_3 | GPIO_Pin_2 | GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_21;
    const uint32_t cols = GPIO_Pin_5 | GPIO_Pin_4 | GPIO_Pin_6 | GPIO_Pin_7;
    const uint32_t knob[] = { GPIO_Pin_14, GPIO_Pin_15, GPIO_Pin_8, GPIO_Pin_9 };

    R16_PIN_ALTERNATE &= ~(RB_RF_ANT_SW_EN | RB_PIN_I2C | RB_PIN_UART3);
    R16_PIN_CONFIG &= ~(RB_PIN_USB_EN | RB_UDP_PU_EN);
    GPIOBDigitalCfg(ENABLE, GPIO_Pin_10 | GPIO_Pin_11);

    GPIOB_ResetBits(rows);
    GPIOB_ModeCfg(rows, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(cols, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_10 | GPIO_Pin_11, GPIO_ModeIN_Floating);
    GPIOB_ModeCfg(GPIO_Pin_16 | GPIO_Pin_17, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_14 | GPIO_Pin_15 | GPIO_Pin_8 | GPIO_Pin_9, GPIO_ModeIN_PU);
    mDelaymS(2);
    for (uint8_t i = 0; i < sizeof(knob) / sizeof(knob[0]); i++) {
        if (!GPIOB_ReadPortPin(knob[i])) {
            GPIOB_ModeCfg(knob[i], GPIO_ModeIN_PD);
        }
    }
    GPIOA_ModeCfg(GPIO_Pin_6, GPIO_ModeIN_Floating);
}
