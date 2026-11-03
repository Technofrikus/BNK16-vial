/*
 * ble_app.h — BLE 蓝牙应用层核心
 *
 * 负责 BLE 协议栈初始化、GAP 事件处理、HID 报告发送。
 * 基于沁恒官方 HID_Keyboard 示例移植，适配 QMK 主循环。
 */

#ifndef __BLE_APP_H
#define __BLE_APP_H

#include <stdint.h>
#include <stdbool.h>

/* ========================================================================
 * BLE 连接状态
 * ======================================================================== */

typedef enum {
    BLE_STATE_IDLE = 0,        /* 空闲 — 未连接，未广播 */
    BLE_STATE_ADVERTISING,     /* 广播中 — 被动等待新设备连接 */
    BLE_STATE_RECONNECTING,    /* 回连中 — 先定向广播，超时转普通广播 */
    BLE_STATE_PAIRING,         /* 配对中 — 可被新设备搜索配对，超时保持原绑定 */
    BLE_STATE_CONNECTING,      /* 连接中 — 正在建立连接 */
    BLE_STATE_CONNECTED,       /* 已连接 — 正常工作 */
    BLE_STATE_STANDBY,         /* 待机 — 蓝牙关闭，等用户按BT键唤醒 */
    BLE_STATE_SWITCHING,       /* 切换中 — 主动切换设备的中间态，避免GAPROLE_WAITING误判为被动断开 */
} ble_state_t;

/* ========================================================================
 * API 函数
 * ======================================================================== */

/* 初始化 BLE 协议栈和 HID 服务
 * 包括：协议栈初始化、GAP 外设角色、HID 服务注册、广播参数设置
 */
void ble_app_init(void);

/* Periodic work outside the stack: reconnect and pairing timeouts, deferred bond table save.
 * Call from housekeeping. TMOS_SystemProcess() runs in platform_run(), not here.
 */
void ble_app_poll(void);

/* Pairing progress from hiddev.c: marks the current slot as paired on success */
void ble_app_pair_state(uint8_t state, uint8_t status);

/* 发送键盘 HID 报告到蓝牙主机
 * 参数：
 *   modifiers - 修饰键位（Ctrl/Shift/Alt/GUI）
 *   keys - 6个键码数组
 * 报告格式：标准 USB HID 键盘报告（8字节）
 */
void ble_send_keyboard_report(uint8_t modifiers, uint8_t *keys);

/* Mouse report without ID: buttons, X, Y, wheel, AC pan */
void ble_send_mouse_report(uint8_t *report, uint8_t len);

/* System control or consumer report, report_id is HID_RPT_ID_SYSTEM_IN or HID_RPT_ID_CONSUMER_IN */
void ble_send_extra_report(uint8_t report_id, uint16_t usage);

/* wait_ms() keeps BLE running only while the QMK task runs in the main loop */
void ble_qmk_task_running(bool running);
void ble_wait_ms(uint32_t ms);

/* 发送空键盘报告（释放所有键）*/
void ble_send_keyboard_release(void);

/* 重试待发送的HID报告（主循环里调用）*/
void ble_retry_pending_report(void);

/* What was asked of the host and what the link really uses, for BNK16_INTERVAL_DEBUG. Units as in BLE. */
typedef struct {
    uint16_t requests;
    uint16_t requested_max;
    uint8_t request_status;
    uint16_t updates;
    uint16_t interval;
    uint16_t latency;
    uint16_t timeout;
} ble_conn_debug_t;
extern ble_conn_debug_t ble_conn_debug;

/* 获取当前 BLE 连接状态 */
ble_state_t ble_get_state(void);

/* 判断是否已连接 */
bool ble_is_connected(void);

/* 进入可配对模式（开始广播，允许新设备配对）*/
void ble_start_advertising(void);

/* 停止广播（USB 连接时调用，省电）*/
void ble_stop_advertising(void);

/* Wired mode: disconnect, stop advertising, drop pending slot changes */
void ble_radio_off(void);

/* Key press in BT mode: restart the current slot if BLE gave up (reconnect timeout) */
void ble_wake_from_standby(void);

/* 断开当前连接 */
void ble_disconnect(void);

/* Switch to a slot (1/2/3): reconnect if it has a bond, otherwise open for pairing.
 * Does nothing in wired mode.
 */
void ble_switch_device(uint8_t device_idx);

/* Open a slot (1/2/3) for pairing. The old bond stays until a new pairing replaces it.
 * Does nothing in wired mode.
 */
void ble_pair_device(uint8_t device_idx);

/* 清除所有蓝牙配对信息 */
void ble_clear_bonds(void);

/* 获取当前连接句柄（内部用）*/
uint16_t ble_get_conn_handle(void);

/* ========================================================================
 * Slot memory. The mode is not stored: the board code decides it.
 * ======================================================================== */

/* Selected slot (1-3) */
uint8_t ble_get_current_device(void);

/* Called from main after init: restores the slot. It starts on GAPROLE_STARTED, if not in wired mode. */
void ble_restore_last_mode(void);

/* 连接间隔动态调整
 * 档位0~4 对应 100/300/500/800/1000ms，电源管理模块按睡眠时长调用
 */

/* 请求切换连接间隔档位（0~4），带5秒防抖 */
void ble_request_conn_interval(uint8_t level);

/* 强制切换连接间隔档位（绕过防抖，用于携带模式切换）*/
void ble_request_conn_interval_force(uint8_t level);

/* 直接设置连接间隔（单位1.25ms，用于携带模式2000ms=1600）*/
void ble_set_conn_interval_raw(uint16_t interval);

/* 获取当前连接间隔档位（0~3）*/
uint8_t ble_get_current_interval_level(void);

#endif /* __BLE_APP_H */
