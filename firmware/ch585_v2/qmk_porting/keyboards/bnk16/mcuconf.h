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
#ifndef HAL_SLEEP_XT_PRE
#define HAL_SLEEP_XT_PRE     1
#endif
/* Test variants (build with BNK16_DEFS, see rules.cmake); none is part of the normal firmware:
 *   BNK16_FAULT_DIAG                crash and hang reports, watchdogs in deep sleep (bnk16_sleep_diag.c)
 *   BNK16_BOOT_DIAG                 reset cause as a status LED colour for 4 s after boot (same file)
 *   BNK16_REGDUMP                   register snapshot at the first deep sleep (same file)
 *   BNK16_DEEP_SLEEP_NO_CHIP_SLEEP  deep sleep board side only (USB off, pins armed, no QMK task), the
 *                                   chip itself never sleeps (hal_sleep_allowed() always false)
 *   HAL_SLEEP_XT_PRE=0              see above
 *   HAL_BLE_IRQ_PRIORITY            PFIC_SetPriority(BLEL_IRQn, 0xF0) as in the WCH EVT MCU.c. On in
 *                                   47v-47y, off again from 47z: the link holds without it. */
#define HSE_LOAD_CAPACITANCE 20 // in pF unit
#define LSE_LOAD_CAPACITANCE 19 // in pF unit
