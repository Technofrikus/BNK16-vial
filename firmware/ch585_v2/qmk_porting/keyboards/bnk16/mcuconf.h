/*
 * BNK16 platform config (same values as the CH585 EVB)
 */

#pragma once

/* Serial log off; for logging enable Debug_UART1 (TX = PA9, not verified on the BNK16) */
// #define DEBUG                Debug_UART1
#define DEBUG_BAUDRATE       460800
/* No effect: no code sets RB_PWR_DCDC_EN, see docs/bnk16/PROJECT_NOTES.md */
#define DCDC_ENABLE          1
#define FREQ_SYS             62400000
#define LSE_ENABLE           0
#define BLE_SLOT_NUM         4
/* Deep sleep with the BLE link kept (phase 47): while asleep the BLE stack sleeps the chip between radio
 * events (CH58X_LowPower in sdk/HAL/SLEEP.c, only while power_management.c allows it) and the key and
 * knob pins wake it, see bnk16.c "Deep sleep and shutdown". Comment out to go back to the light sleep
 * (WFI, about 8.4 mA) of phase 46b; the shutdown works either way. Defined here, not in qmk_config.h,
 * because the SDK HAL needs HAL_SLEEP too. */
#define BNK16_DEEP_SLEEP
#ifdef BNK16_DEEP_SLEEP
#define HAL_SLEEP            1
#else
#define HAL_SLEEP            0
#endif
#define WAKE_UP_RTC_MAX_TIME US_TO_RTC(1600) /* CH585 EVT value (the CH582 SDK copy here has 1400 us) */
/* 1: sleep as the newest WCH EVT, with the crystal pre-start on newer chip revisions; 0: always the
 * path of the GitHub EVT that test B ran (see sdk/HAL/SLEEP.c) */
/* HAL_BLE_IRQ_PRIORITY (BLE link layer interrupt priority as in the WCH EVT) was on in 47v-47y and is off
 * again from 47z: the one hang of those builds (47w, in the library's ll_rx_wait_finish) came with it,
 * and the fix for the lost link was the main loop (47y). Test variant only. */
#ifndef HAL_SLEEP_XT_PRE
#define HAL_SLEEP_XT_PRE     1
#endif
/* Test variants (build with BNK16_DEFS, see rules.cmake):
 *   BNK16_DEEP_SLEEP_NO_CHIP_SLEEP  deep sleep board side only (USB off, pins armed, no QMK task), the
 *                                   chip itself never sleeps (hal_sleep_allowed() always false)
 *   HAL_BLE_IRQ_PRIORITY            PFIC_SetPriority(BLEL_IRQn, 0xF0) as in the WCH EVT MCU.c
 *   BNK16_BOOT_DIAG                 reset cause as a status LED colour for 4 s after boot (bnk16.c) */
#define HSE_LOAD_CAPACITANCE 20 // in pF unit
#define LSE_LOAD_CAPACITANCE 19 // in pF unit
