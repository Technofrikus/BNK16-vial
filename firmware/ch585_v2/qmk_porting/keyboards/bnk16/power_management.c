/*
 * power_management.c — idle sleep and shutdown on battery, see power_management.h
 */

#include "power_management.h"
#include "battery.h"
#include "ble_app.h"
#include "ble_config.h"
#include "timer.h"
#include "bnk16.h"
#include "CH58x_common.h"
#include "CH58xBLE_LIB.h"

static power_state_t current_power_state = POWER_STATE_ACTIVE;
static uint32_t      last_activity_time;
static uint32_t      sleep_start_time;
static bool          s_wired_mode;
#ifdef BNK16_DEEP_SLEEP
static bool          s_deep_sleep;
static uint32_t      deep_sleep_exit_time;
static uint32_t      deep_sleep_wait_ms; /* QMK task time before the next deep sleep may start */
/* Light sleep: the QMK task keeps running (matrix scan every 4 ms, so all knobs work) this long after the
 * board falls asleep, then the first deep sleep follows. It is a setting (bnk16.c, three keycodes, 1 s to
 * 120 min, default 5 min). Phase 47w: even the shortest value gives the QMK task time to switch the LEDs
 * off (suspend, supply) before the deep sleep, and in 47v the link held in the deep sleeps that followed
 * an exit but never in the first one. A test build can pin it with -DPOWER_DEEP_SLEEP_SETTLE_MS=<ms>. */
#ifdef POWER_DEEP_SLEEP_SETTLE_MS
#define light_sleep_ms() ((uint32_t)POWER_DEEP_SLEEP_SETTLE_MS)
#else
#define light_sleep_ms() bnk16_light_sleep_ms()
#endif
#endif

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
#ifdef BNK16_DEEP_SLEEP
    deep_sleep_exit_time = sleep_start_time;
    deep_sleep_wait_ms   = light_sleep_ms();
#endif
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

    /* Empty battery: shut down to protect the cell, the same way as after a long idle; a key reboots it.
     * Awake the status LED shows red for 2 s first (bnk16.c). */
    bnk16_battery_empty_task(battery_is_critical() || bnk16_battery_empty_at_boot());

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

/* ========================================================================
 * Deep sleep (BNK16_DEEP_SLEEP): asleep in the BT position, the chip sleeps between radio events and
 * a wake-up pin ends it. Board side in bnk16.c ("Deep sleep and shutdown").
 * ======================================================================== */
#ifdef BNK16_DEEP_SLEEP
/* Asked by CH58X_LowPower() (sdk/HAL/SLEEP.c) each time the BLE stack is idle, with interrupts off */
__HIGH_CODE bool hal_sleep_allowed(void)
{
#ifdef BNK16_DEEP_SLEEP_NO_CHIP_SLEEP
    return false; /* test variant: board side of the deep sleep only */
#endif
    return s_deep_sleep && !bnk16_deep_sleep_woken();
}

bool power_in_deep_sleep(void)
{
    return s_deep_sleep;
}

/* Phase 47y: in deep sleep the main loop is WCH's own, TMOS and nothing else, from RAM. Only a wake-up
 * pin or 250 ms (8000 RTC ticks, read raw: no division, no flash) end it; then the slow pass below runs
 * once. Before, some of our code ran from flash after every wake-up, just ahead of the radio event. */
__HIGH_CODE __attribute__((noinline)) static void deep_sleep_spin(void)
{
    uint32_t start = R32_RTC_CNT_32K;
    do {
        TMOS_SystemProcess();
    } while (!bnk16_deep_sleep_woken() && (uint32_t)(R32_RTC_CNT_32K - start) < 8000);
}

bool power_deep_sleep_task(void)
{
    if (s_deep_sleep) {
        deep_sleep_spin();
        if (!bnk16_deep_sleep_poll()) {
            /* interval step, shutdown and the BLE timeouts still run, but only now and then (phase 47v):
             * the chip wakes just 1.6 ms before a radio event and WCH's own loop does nothing but TMOS */
            static uint32_t last_slow;
            if (timer_elapsed32(last_slow) >= 250) {
                last_slow = timer_read32();
                power_task();
                ble_app_poll();
            }
            return true;
        }
        s_deep_sleep         = false;
        deep_sleep_exit_time = timer_read32();
        deep_sleep_wait_ms   = POWER_DEEP_SLEEP_RETRY_MS;
        bnk16_deep_sleep_exit();
        return false;
    }

    /* After a wake-up without a key (knob jitter) the QMK task runs for a while before the next try;
     * a key in that time wakes the board through power_notify_activity() */
    if (current_power_state == POWER_STATE_SLEEPING && !s_wired_mode &&
        timer_read32() - deep_sleep_exit_time >= deep_sleep_wait_ms && bnk16_shutdown_possible()) {
        bnk16_deep_sleep_enter();
        s_deep_sleep = true;
        return true;
    }
    return false;
}
#endif
