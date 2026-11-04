/*
 * power_management.h — idle sleep and shutdown on battery
 *
 * BT position only (the USB position never sleeps):
 *   awake  -> sleep after POWER_SLEEP_TIMEOUT_MS without a key: LEDs and their supply off, BLE link kept
 *             (15 ms, 100-120 ms after BLE_CONN_INTERVAL_L1_TIME asleep, see ble_config.h); any key wakes
 *   asleep -> shutdown POWER_SHUTDOWN_TIMEOUT_MS after the last key, or at once on an empty battery
 * The core waits in WFI between QMK tasks in every state (platform_run() in protocol_usb.c).
 */

#ifndef __POWER_MANAGEMENT_H
#define __POWER_MANAGEMENT_H

#include <stdint.h>
#include <stdbool.h>

/* Idle time before the board sleeps (LEDs off, link stays up) */
#ifndef POWER_SLEEP_TIMEOUT_MS
#define POWER_SLEEP_TIMEOUT_MS     (5 * 60 * 1000)
#endif

/* Time after the last key until the board shuts down, the way the stock firmware sleeps: the link ends,
 * the chip is off except for the wake-up pins, and a key or knob reboots it (that key is not sent) */
#ifndef POWER_SHUTDOWN_TIMEOUT_MS
#define POWER_SHUTDOWN_TIMEOUT_MS  (30 * 60 * 1000)
#endif

typedef enum {
    POWER_STATE_ACTIVE = 0,
    POWER_STATE_SLEEPING, /* LEDs and their supply off, matrix still scanned (every 4 ms) */
} power_state_t;

void power_init(void);

/* From housekeeping, every pass: timeouts, interval steps, shutdown */
void power_task(void);

/* A key press: restarts the idle time and wakes the board */
void power_notify_activity(void);

power_state_t power_get_state(void);

/* USB position: no sleep and no shutdown */
void power_set_wired_mode(bool wired);

#endif /* __POWER_MANAGEMENT_H */
