/*
 * Status LED API of the shared BLE and USB code. The BNK16 has no discrete status LEDs (PB14/PB15 are
 * the small left knob here), so led_indicator.c implements it as no-ops; the status LED in the RGB
 * chain is driven by bnk16.c.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* LED0 连接状态 */
typedef enum {
    LED_CONN_IDLE = 0,          /* 熄灭（正常使用） */
    LED_CONN_WIRED,              /* 有线插入：常亮2s后熄灭 */
    LED_CONN_PAIRING,            /* 配对中：快闪3Hz */
    LED_CONN_RECONNECTING,       /* 回连中：慢闪1Hz */
    LED_CONN_SUCCESS,            /* 连接成功：常亮2s后熄灭 */
} led_conn_state_t;

/* LED1 电源状态 */
typedef enum {
    LED_PWR_IDLE = 0,            /* 熄灭（正常使用） */
    LED_PWR_CHARGING,            /* 充电中：常亮 */
    LED_PWR_CHARGED,             /* 充满：熄灭 */
    LED_PWR_LOW_BATTERY,         /* 低电量：慢闪1次/3s */
} led_pwr_state_t;

/* 初始化LED硬件 */
void led_indicator_init(void);

/* 主循环调用，非阻塞 */
void led_indicator_task(void);

/* 设置LED0连接状态 */
void led_set_conn_state(led_conn_state_t state);

/* 设置LED1电源状态 */
void led_set_pwr_state(led_pwr_state_t state);
