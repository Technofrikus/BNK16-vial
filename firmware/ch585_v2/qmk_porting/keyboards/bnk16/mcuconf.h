/*
 * BNK16 platform config (same values as the CH585 EVB)
 */

#pragma once

/* Serial log off; for logging enable Debug_UART1 (TX = PA9, not verified on the BNK16) */
// #define DEBUG                Debug_UART1
#define DEBUG_BAUDRATE       460800
#define DCDC_ENABLE          1
#define FREQ_SYS             62400000
#define LSE_ENABLE           0
#define BLE_SLOT_NUM         4
/* 1 = the BLE stack sleeps the core between radio events (CH58X_LowPower), only while power_management.c
 * allows it, see hal_sleep_allowed(). Needs BNK16_DEEP_SLEEP in qmk_config.h; off, see there. */
#define HAL_SLEEP            0
#define WAKE_UP_RTC_MAX_TIME US_TO_RTC(1600) /* official CH585 EVT value (the CH582 SDK copy here has 1400 us) */
#define HSE_LOAD_CAPACITANCE 20 // in pF unit
#define LSE_LOAD_CAPACITANCE 19 // in pF unit
