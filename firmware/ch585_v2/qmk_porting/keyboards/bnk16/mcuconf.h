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
#define HSE_LOAD_CAPACITANCE 20 // in pF unit
#define LSE_LOAD_CAPACITANCE 19 // in pF unit
