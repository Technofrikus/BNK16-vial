/*
 * BNK16: no discrete status LEDs.
 * The EVB drove PB14/PB15 here, but on the BNK16 those pins are the VBUS
 * detect input and the USB/BT mode switch, so they must never be driven.
 * The API is kept as no-ops because the BLE and power code call into it.
 */
#include "led_indicator.h"

void led_indicator_init(void) {}
void led_indicator_task(void) {}
void led_set_conn_state(led_conn_state_t state) { (void)state; }
void led_set_pwr_state(led_pwr_state_t state) { (void)state; }
