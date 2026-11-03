/*
 * power_management.h — 电源管理
 * Sleep after POWER_SLEEP_TIMEOUT_MS without a key (LEDs and their supply off, link stays up), any key
 * wakes; shutdown after POWER_SHUTDOWN_TIMEOUT_MS; no sleep in wired mode. WFI between QMK tasks. BLE interval 15 ms, 100-120 ms after a while
 * asleep (ble_config.h). Low battery: LED1 blinks slowly.
 */

#ifndef __POWER_MANAGEMENT_H
#define __POWER_MANAGEMENT_H

#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * 配置参数
 * ======================================================================== */

/* Idle time before the board sleeps (LEDs off, link stays up) */
#ifndef POWER_SLEEP_TIMEOUT_MS
#define POWER_SLEEP_TIMEOUT_MS     (5 * 60 * 1000)
#endif

/* Time after the last key until the board shuts down, the way the stock firmware sleeps: the link ends,
 * the chip is off except for the wake-up pins, and a key or knob reboots it (that key is not sent) */
#ifndef POWER_SHUTDOWN_TIMEOUT_MS
#define POWER_SHUTDOWN_TIMEOUT_MS  (30 * 60 * 1000)
#endif

/* After a deep sleep wake-up without a key (knob jitter, layer switch), the matrix gets this long to
 * see something before the core sleeps again; longer than the debounce */
#define POWER_DEEP_SLEEP_RETRY_MS  100

/* 低电量阈值（百分比）— 低于这个值 LED1 慢闪提醒 */
#define POWER_LOW_BATTERY_THRESHOLD  10  /* 10% */

/* ========================================================================
 * 电源状态
 * ======================================================================== */

typedef enum {
    POWER_STATE_ACTIVE = 0,   /* 正常工作 — LED1常亮，矩阵扫描正常 */
    POWER_STATE_SLEEPING,      /* 浅睡眠 — LED全灭，矩阵扫描停止，WFI等待中断 */
} power_state_t;

/* ========================================================================
 * API 函数
 * ======================================================================== */

/* 初始化电源管理 — 设置初始状态为 ACTIVE，启动无操作计时 */
void power_init(void);

/* 主循环轮询 — 检查无操作超时，超时则进入睡眠
 * 需要在主循环里定期调用
 */
void power_task(void);

/* 通知电源管理有用户活动（按键按下等）— 重置无操作计时
 * 在矩阵扫描检测到按键时调用
 */
void power_notify_activity(void);

/* 手动进入睡眠 — 立即进入浅睡眠（不等超时）
 * 一般不需要手动调用，超时自动进入
 */
void power_enter_sleep(void);

/* 手动唤醒 — 从睡眠中唤醒
 * 一般由按键中断自动调用，不需要手动调用
 */
void power_wake_up(void);

/* 获取当前电源状态 */
power_state_t power_get_state(void);

/* Deep sleep, BT mode only, called from the main loop every pass. While asleep with no USB host it
 * hands the pins to the wake-up interrupts and lets the BLE stack stop the core between radio events.
 * Returns true while in deep sleep: the caller then skips the QMK task. */
bool power_deep_sleep_task(void);

/* Deep sleep entries and exits since boot, for BNK16_SLEEP_DIAG */
extern uint16_t power_deep_sleep_entries;
extern uint16_t power_deep_sleep_exits;

/* 获取距离睡眠还有多少时间（毫秒）— 用于调试 */
uint32_t power_get_time_until_sleep(void);

/* 设置是否为有线模式 — 有线模式下不进睡眠 */
void power_set_wired_mode(bool wired);

/* 设置是否为携带模式 — 携带模式下禁用阶梯式连接间隔调整（固定2000ms）*/
void power_set_mobile_mode(bool enabled);

/* 查询是否为携带模式 */
bool power_is_mobile_mode(void);

#endif /* __POWER_MANAGEMENT_H */
