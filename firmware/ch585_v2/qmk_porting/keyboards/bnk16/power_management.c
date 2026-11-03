/*
 * power_management.c — 电源管理
 * 功能：无操作进睡眠，按键唤醒；有线模式不进睡眠；
 *       睡眠时关LED，低电量LED1慢闪；WFI低功耗；
 *       Connection interval by time asleep: fast, slow (ble_config.h)
 */

#include "power_management.h"
#include "battery.h"
#include "ble_app.h"
#include "ble_config.h"
#include "timer.h"
#include "led_indicator.h"
#include "CH58x_common.h"
#include "bnk16.h"
#include "bnk16_diag.h"
#ifdef BNK16_POWER_DIAG
#include "battery_measure.h"
#endif

/* ========================================================================
 * 内部状态
 * ======================================================================== */

static power_state_t current_power_state = POWER_STATE_ACTIVE;
static uint32_t last_activity_time = 0;
static bool low_battery_active = false;
static bool s_wired_mode = false;
static bool s_mobile_mode = false;  /* 携带模式：禁用阶梯式连接间隔，固定2000ms */
static uint32_t sleep_start_time = 0;
static bool s_deep_sleep = false;
static uint32_t deep_sleep_exit_time = 0;
uint16_t power_deep_sleep_entries = 0;
uint16_t power_deep_sleep_exits = 0;
#ifdef BNK16_POWER_DIAG
static bool s_knob_slow = false;
#endif

/* ========================================================================
 * 初始化
 * ======================================================================== */

void power_init(void)
{
    current_power_state = POWER_STATE_ACTIVE;
    last_activity_time = timer_read32();
    low_battery_active = false;

    /* LED0 连接状态灯由 led_indicator 管理 */
}

/* ========================================================================
 * 主循环轮询
 * ======================================================================== */

void power_task(void)
{
    uint32_t now = timer_read32();

    /* 低电量检测 — 蓝牙模式下低电量时 LED1 慢闪提醒
     * 有线模式下 LED1 常亮（表示外部供电），不受低电量影响
     */
    uint8_t battery_level = battery_get_level();
    bool is_low_battery = (battery_level < POWER_LOW_BATTERY_THRESHOLD);
    if (is_low_battery != low_battery_active) {
        low_battery_active = is_low_battery;
        /* 只在蓝牙模式下更新 LED1 状态，有线模式保持常亮 */
        if (!s_wired_mode) {
            if (is_low_battery) {
                led_set_pwr_state(LED_PWR_LOW_BATTERY);  /* 低电量慢闪 */
            } else {
                led_set_pwr_state(LED_PWR_IDLE);  /* 正常电量熄灭 */
            }
        }
    }

    /* 睡眠状态：按睡眠时长阶梯式切换蓝牙连接间隔
     * 携带模式下跳过（固定2000ms，由携带模式进入时设置）
     */
    if (current_power_state == POWER_STATE_SLEEPING && !s_mobile_mode) {
        uint32_t sleep_duration = now - sleep_start_time;
        uint8_t target_level = sleep_duration >= BLE_CONN_INTERVAL_L1_TIME ? 1 : 0;

#ifdef BNK16_POWER_DIAG
        /* one more thing off per minute, to find where the sleep current goes; the last stage needs a power cycle */
        if (sleep_duration >= BNK16_POWER_DIAG_OFF_TIME) {
            battery_handle_critical();
        }
        if (sleep_duration >= BNK16_POWER_DIAG_KNOB_TIME && !s_knob_slow) {
            s_knob_slow = true;
            bnk16_knob_timer(false);
        }
#endif

        if (target_level != ble_get_current_interval_level()) {
            ble_request_conn_interval(target_level);
        }
        if (now - last_activity_time >= POWER_SHUTDOWN_TIMEOUT_MS && bnk16_deep_sleep_possible()) {
            bnk16_shutdown();
        }
        return;
    }

    /* 有线模式不进睡眠 */
    if (s_wired_mode) {
        last_activity_time = now;
        return;
    }

    /* Awake: the request on wake-up is dropped if the last one was under 5 s ago, so repeat it */
    if (!s_mobile_mode && ble_get_current_interval_level() != 0) {
        ble_request_conn_interval(0);
    }

    /* 无操作超时进睡眠 */
    uint32_t elapsed = now - last_activity_time;
    if (elapsed >= POWER_SLEEP_TIMEOUT_MS) {
        power_enter_sleep();
    }
}

/* ========================================================================
 * 用户活动通知 — 按键按下时调用，重置无操作计时
 * ======================================================================== */

void power_notify_activity(void)
{
    last_activity_time = timer_read32();

    /* 如果在睡眠状态，唤醒 */
    if (current_power_state == POWER_STATE_SLEEPING) {
        power_wake_up();
    }
}

/* ========================================================================
 * 进入睡眠
 * ======================================================================== */

void power_enter_sleep(void)
{
    if (current_power_state == POWER_STATE_SLEEPING) return;

    current_power_state = POWER_STATE_SLEEPING;
    sleep_start_time = timer_read32();
    ble_request_conn_interval(0);  /* 刚睡眠保持低延迟 */

    /* LED0 连接状态灯由 led_indicator 管理，睡眠时自动熄灭 */
    low_battery_active = false;
}

void power_wake_up(void)
{
    if (current_power_state == POWER_STATE_ACTIVE) return;

    current_power_state = POWER_STATE_ACTIVE;
    last_activity_time = timer_read32();
    ble_request_conn_interval(0);  /* 唤醒恢复低延迟 */
#ifdef BNK16_POWER_DIAG
    if (s_knob_slow) {
        s_knob_slow = false;
        bnk16_knob_timer(true);
    }
#endif

    /* LED0 连接状态灯由 led_indicator 管理 */
}

/* ========================================================================
 * Deep sleep
 * ======================================================================== */

/* Read by CH58X_LowPower() (sdk/HAL/SLEEP.c) each time the BLE stack is idle */
__HIGH_CODE bool hal_sleep_allowed(void)
{
#ifdef BNK16_SLEEP_DIAG
    /* the first deep sleep runs without the chip sleeping, to tell the two parts apart */
    if (power_deep_sleep_entries < 2) {
        return false;
    }
#endif
    return s_deep_sleep && !bnk16_deep_sleep_woken();
}

bool power_deep_sleep_task(void)
{
#ifndef BNK16_DEEP_SLEEP
    return false;
#endif
    if (s_deep_sleep) {
        if (!bnk16_deep_sleep_woken()) {
            /* interval step and the BLE timeouts still run, once per radio event */
            power_task();
            ble_app_poll();
            return true;
        }
        s_deep_sleep = false;
        power_deep_sleep_exits++;
        deep_sleep_exit_time = timer_read32();
        bnk16_deep_sleep_exit();
        DIAG_CRUMB(DIAG_CRUMB_DEEP_EXIT);
        return false;
    }

    if (current_power_state == POWER_STATE_SLEEPING && !s_wired_mode &&
        timer_read32() - deep_sleep_exit_time >= POWER_DEEP_SLEEP_RETRY_MS && bnk16_deep_sleep_possible()) {
        bnk16_deep_sleep_enter();
        s_deep_sleep = true;
        power_deep_sleep_entries++;
        DIAG_CRUMB(DIAG_CRUMB_DEEP_ENTER);
        return true;
    }
    return false;
}

/* ========================================================================
 * 状态查询
 * ======================================================================== */

power_state_t power_get_state(void)
{
    return current_power_state;
}

uint32_t power_get_time_until_sleep(void)
{
    if (current_power_state == POWER_STATE_SLEEPING) {
        return 0;
    }
    uint32_t elapsed = timer_read32() - last_activity_time;
    if (elapsed >= POWER_SLEEP_TIMEOUT_MS) {
        return 0;
    }
    return POWER_SLEEP_TIMEOUT_MS - elapsed;
}

/* ========================================================================
 * 有线模式设置 — 有线模式下不进睡眠
 * ======================================================================== */

void power_set_wired_mode(bool wired)
{
    s_wired_mode = wired;

    if (wired) {
        if (current_power_state == POWER_STATE_SLEEPING) {
            power_wake_up();
        }
        last_activity_time = timer_read32();
    } else {
        last_activity_time = timer_read32();
    }
}

/* 携带模式设置 — 携带模式下禁用阶梯式连接间隔（固定2000ms）*/
void power_set_mobile_mode(bool enabled)
{
    s_mobile_mode = enabled;
}

bool power_is_mobile_mode(void)
{
    return s_mobile_mode;
}
