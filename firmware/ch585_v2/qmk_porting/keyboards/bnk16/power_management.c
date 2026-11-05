/*
 * power_management.c — idle sleep and shutdown on battery, see power_management.h
 */

#include "power_management.h"
#include "battery.h"
#include "ble_app.h"
#include "ble_config.h"
#include "timer.h"
#include "bnk16.h"

static power_state_t current_power_state = POWER_STATE_ACTIVE;
static uint32_t      last_activity_time;
static uint32_t      sleep_start_time;
static bool          s_wired_mode;

void power_init(void)
{
    current_power_state = POWER_STATE_ACTIVE;
    last_activity_time  = timer_read32();
}

static void power_enter_sleep(void)
{
    current_power_state = POWER_STATE_SLEEPING;
    sleep_start_time    = timer_read32();
    ble_request_conn_interval(0); /* stays fast for the first part of the sleep */
}

static void power_wake_up(void)
{
    current_power_state = POWER_STATE_ACTIVE;
    last_activity_time  = timer_read32();
    ble_request_conn_interval(0);
}

void power_task(void)
{
    uint32_t now = timer_read32();

    /* USB position: the switch cuts the battery, never sleep */
    if (s_wired_mode) {
        last_activity_time = now;
        return;
    }

    /* Empty battery: shut down to protect the cell, the same way as after a long idle; a key reboots it */
    if (battery_is_critical() && bnk16_shutdown_possible()) {
        bnk16_shutdown();
    }

    if (current_power_state == POWER_STATE_SLEEPING) {
        /* slower BLE interval after a while asleep */
        uint8_t target_level = now - sleep_start_time >= BLE_CONN_INTERVAL_L1_TIME ? 1 : 0;
        if (target_level != ble_get_current_interval_level()) {
            ble_request_conn_interval(target_level);
        }
        /* no idle shutdown while the battery charges, the cable is in; once it is full (or the cable is
         * pulled) the voltage stops holding at the charging level and the shutdown follows */
        if (now - last_activity_time >= POWER_SHUTDOWN_TIMEOUT_MS && !battery_is_charging() &&
            bnk16_shutdown_possible()) {
            bnk16_shutdown();
        }
        return;
    }

    /* Awake: the request on wake-up is dropped if the last one was under 5 s ago, so repeat it */
    if (ble_get_current_interval_level() != 0) {
        ble_request_conn_interval(0);
    }

    if (now - last_activity_time >= POWER_SLEEP_TIMEOUT_MS) {
        power_enter_sleep();
    }
}

void power_notify_activity(void)
{
    last_activity_time = timer_read32();
    if (current_power_state == POWER_STATE_SLEEPING) {
        power_wake_up();
    }
}

power_state_t power_get_state(void)
{
    return current_power_state;
}

void power_set_wired_mode(bool wired)
{
    s_wired_mode = wired;
    if (wired && current_power_state == POWER_STATE_SLEEPING) {
        power_wake_up();
    }
    last_activity_time = timer_read32();
}
