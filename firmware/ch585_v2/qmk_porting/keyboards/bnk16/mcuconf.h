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
/* No chip sleep between radio events: on this board it lost power or never woke up (phase 38 in
 * docs/bnk16/PROJECT_NOTES.md). The board shuts down instead, see power_management.h. */
#define HAL_SLEEP            0
#define HSE_LOAD_CAPACITANCE 20 // in pF unit
#define LSE_LOAD_CAPACITANCE 19 // in pF unit
