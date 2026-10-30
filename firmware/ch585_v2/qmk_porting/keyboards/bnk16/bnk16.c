/*
 * Binepad BNK16 board entry (based on the CH585 EVB entry)
 * Pin map and open points: docs/bnk16/HARDWARE.md
 *
 * 模式状态机（参考量产键盘逻辑）：
 *   WIRED（有线模式）：USB 已连接，停止蓝牙广播，LED 亮2秒提示后熄灭
 *   BLE_IDLE（无线空闲）：USB 未连接，没有主动连接蓝牙，LED 灭
 *   BLE_RECONNECTING（回连中）：定向广播→普通广播，LED 慢闪
 *   BLE_PAIRING（配对中）：普通广播可被搜索，LED 快闪
 *   BLE_CONNECTED（已连接）：正常工作，LED 亮2秒后熄灭
 *   BLE_STANDBY（待机）：回连/配对超时后进入，LED 灭，按 BT 键唤醒
 *
 * 用户逻辑（参考量产键盘）：
 *   上电 → 恢复上次模式（有线/蓝牙），蓝牙模式自动回连上次设备
 *   运行中插 USB → 自动切有线（断开蓝牙，停止广播）
 *   运行中拔 USB → 不自动切无线，保持当前状态
 *   短按 BT_1/2/3 → 切到蓝牙模式，切换到对应设备（有配对则回连，无配对则广播）
 *   长按 BT_1/2/3（≥3秒）→ 进入配对模式（不删旧绑定，新配对覆盖）
 *   按 WIRED_MODE → 切到有线模式
 *   按 BT_CLEAR → 清除所有配对
 *   模式和设备记忆到 EEPROM，上电恢复
 */

#include QMK_KEYBOARD_H
#include "ble_app.h"
#include "usb_device_state.h"
#include "power_management.h"
#include "battery.h"
#include "led_indicator.h"
#include "bnk16_diag.h"
#include "CH58x_common.h"

/* 有线模式激活标志 — 全局变量，供 ble_app.c 里的状态回调检查
 * 有线模式下，蓝牙状态变化不应覆盖 LED 状态
 */
bool wired_mode_active = false;

/* 记录上一次的 USB 状态，用于检测状态变化 */
static bool last_usb_connected = false;

/* USB connected and enumerated. The port never reports to QMK's usb_device_state (it always reads
 * INIT), so read the USBHS driver state directly: EnumStatus is set by SET_CONFIGURATION and cleared
 * by a bus reset; bit 1 of SleepStatus is the bus suspend, which is also what an unplugged cable looks
 * like (no VBUS pin is known). A host in standby therefore also counts as not connected. */
extern volatile uint8_t USBHS_DevEnumStatus;
extern volatile uint8_t USBHS_DevSleepStatus;

static bool usb_is_connected(void) {
    return USBHS_DevEnumStatus && !(USBHS_DevSleepStatus & 0x02);
}

/* 切换到有线模式：断开蓝牙，停止广播，LED 亮2秒提示 */
void enter_wired_mode(void) {
    /* 设置有线模式标志 — 必须在断开蓝牙之前设置，
     * 否则 ble_state_cb 里的状态变化会覆盖 LED 状态
     */
    wired_mode_active = true;

    /* 通知电源管理进入有线模式 — 有线模式不进睡眠 */
    power_set_wired_mode(true);

    /* 保存模式记忆：有线模式 */
    ble_save_mode(true, 1);

    /* 断开蓝牙连接（如果已连接）*/
    if (ble_is_connected()) {
        ble_disconnect();
    }

    /* 停止蓝牙广播 */
    ble_stop_advertising();

    /* LED1 电源灯：有线模式常亮（表示外部供电）*/
    led_set_pwr_state(LED_PWR_CHARGING);

    /* LED0 连接状态灯由 led_indicator 管理 */
}

/* 退出有线模式：清除标志（蓝牙模式由 ble_app.c 管理 LED）*/
static void exit_wired_mode(void) {
    wired_mode_active = false;

    /* 通知电源管理退出有线模式 — 无线模式下可以进睡眠 */
    power_set_wired_mode(false);

    /* LED1 电源灯：蓝牙模式熄灭（电池供电，省电）
     * 低电量时由 power_management.c 统一设置为慢闪
     */
    led_set_pwr_state(LED_PWR_IDLE);
}

/* ========================================================================
 * 主循环里的 USB 状态检测和模式切换
 *
 * QMK 会在主循环里定期调用 housekeeping_task_kb()
 * 这里检测 USB 状态变化，触发模式切换
 * ======================================================================== */

/* 4-position layer switch: its two contacts are matrix (0,4) and (1,4) (column PB10, diodes to
 * rows PB3/PB2), measured: none = layer 0, (0,4) = 1, (1,4) = 2, both = 3. The keymap has KC_NO
 * there; the layer is only moved when the switch position changes, so MO()/TG() keys keep working
 * on top of it. The matrix is debounced, so the value is stable here. */
static void layer_switch_task(void) {
    static uint8_t last = 0xFF;
    uint8_t        pos  = (matrix_is_on(0, 4) ? 1 : 0) | (matrix_is_on(1, 4) ? 2 : 0);

    if (pos != last && timer_read32() > 100) {
        last = pos;
        layer_move(pos < DYNAMIC_KEYMAP_LAYER_COUNT ? pos : 0);
    }
}

void housekeeping_task_kb(void) {
    layer_switch_task();
    bnk16_diag_task();
    /* 电源管理任务 — 检查无操作超时、低电量提醒 */
    power_task();

    /* the BLE stack may enable the RF antenna switch outputs (PB16-PB21) later on; PB21 is a matrix row */
    if (R16_PIN_ALTERNATE & RB_RF_ANT_SW_EN) {
        R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    }

    bool current_usb = usb_is_connected();

    /* USB 状态变化检测 */
    if (current_usb != last_usb_connected) {
        if (current_usb) {
            /* USB 从断开 → 连接：自动切换到有线模式 */
            enter_wired_mode();
        } else {
            /* USB 从连接 → 断开：不自动切无线，保持当前状态
             * 用户需要手动按 BT_1/2/3 才会切到蓝牙
             */
            exit_wired_mode();
            /* LED0 连接状态灯由 led_indicator 管理 */
        }
        last_usb_connected = current_usb;
    }

    /* 有线模式下持续确保蓝牙广播已停止（防止 BLE 协议栈内部重新启动广播）*/
    if (wired_mode_active) {
        if (ble_get_state() == BLE_STATE_ADVERTISING || ble_get_state() == BLE_STATE_PAIRING) {
            ble_stop_advertising();
        }
        /* 注意：有线模式下 LED 不常亮，只在进入时亮2秒提示，之后熄灭（省电且不干扰）*/
    }
}

#ifdef RGB_MATRIX_ENABLE
/* ========================================================================
 * RGB matrix: LEDs 0-15 are the keys (row-major, matrix row * 4 + col),
 * LED BNK16_STATUS_LED is the extra status LED. It has no flags, so no
 * effect touches it; it is driven only from the indicator callback below.
 * Encoder clicks (matrix row 4) have no LED.
 * ======================================================================== */
led_config_t g_led_config = {
    {
        { 0, 1, 2, 3, NO_LED },
        { 4, 5, 6, 7, NO_LED },
        { 8, 9, 10, 11, NO_LED },
        { 12, 13, 14, 15, NO_LED },
        { NO_LED, NO_LED, NO_LED, NO_LED, NO_LED },
    },
    {
        {   0,  0 }, {  75,  0 }, { 150,  0 }, { 224,  0 },
        {   0, 21 }, {  75, 21 }, { 150, 21 }, { 224, 21 },
        {   0, 43 }, {  75, 43 }, { 150, 43 }, { 224, 43 },
        {   0, 64 }, {  75, 64 }, { 150, 64 }, { 224, 64 },
        { 112, 64 },
    },
    {
        LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL,
        LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL,
        LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL,
        LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL, LED_FLAG_ALL,
        LED_FLAG_NONE,
    }
};

/* Status LED: green = USB enumerated by a host, blue = BLE connected, dim orange = otherwise.
 * Shows the real USB state, not wired_mode_active: that flag is the remembered mode and stays set
 * on battery after a boot that restored wired mode. There is no VBUS pin, so a charger-only supply
 * also shows orange. The port's own rgb_matrix_indicators_advanced_kb() (extra_indicator.c) calls this hook. */
bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    if (BNK16_STATUS_LED >= led_min && BNK16_STATUS_LED < led_max) {
        if (usb_is_connected()) {
            rgb_matrix_set_color(BNK16_STATUS_LED, 0, 40, 0);
        } else if (ble_is_connected()) {
            rgb_matrix_set_color(BNK16_STATUS_LED, 0, 0, 40);
        } else {
            rgb_matrix_set_color(BNK16_STATUS_LED, 40, 16, 0);
        }
    }
    return true;
}
#endif

/* PB14/PB15 are also the TIO/TCK debug pins, and PB16-PB23 have separate digital-input-disable bits
 * (R16_PIN_CONFIG) next to the PB0-PB15 ones (R32_PIN_IN_DIS). Make sure every encoder pin is a plain
 * digital input before QMK's encoder driver configures the pull-ups. */
static void bnk16_encoder_pins_prepare(void) {
    sys_safe_access_enable();
    R8_SAFE_DEBUG_CTRL |= RB_DEBUG_DIS;
    sys_safe_access_disable();
    GPIOBDigitalCfg(ENABLE, GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_14 | GPIO_Pin_15 | GPIO_Pin_16 | GPIO_Pin_17 | GPIO_Pin_21);
    /* PB16-PB21 can be taken over as RF antenna switch outputs; PB21 must stay a plain GPIO (matrix row 4) */
    R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    /* keep SCL/TXD3 off PB21 (both are off after reset; I2C and UART3 are not used) */
    R16_PIN_ALTERNATE &= ~(RB_PIN_I2C | RB_PIN_UART3);
    /* PB10 is matrix column 4 (layer switch common): keep the full speed USB port and its forced
     * UD+ pull-up off PB10/PB11 (USB runs on the HS port only, see usb_config.h) */
    R16_PIN_CONFIG &= ~(RB_PIN_USB_EN | RB_UDP_PU_EN);
    GPIOBDigitalCfg(ENABLE, GPIO_Pin_10 | GPIO_Pin_11);
    /* PB11 = BT/USB slide switch (the switch also cuts battery +), low = BT position.
     * Not evaluated yet: it becomes the mode input once Bluetooth is brought up. */
    gpio_set_pin_input_high(B11);
}

#ifdef ENCODER_ENABLE
/* ========================================================================
 * Big knob (no detents, resolution 1): direction hysteresis.
 * Contact bounce at a transition shows up as one quarter step forth and back.
 * With resolution 1 each of those became an output step in the wrong direction
 * (and a time debounce lost states when turning fast). Here the raw quadrature
 * position is tracked every scan, and a filtered position follows it: in the
 * current direction immediately, against it only once the raw position is two
 * quarter steps away. QMK's decoder is fed the filtered position as a clean
 * Gray code, so a bounce (+-1) never produces output; a knob resting between
 * two positions keeps its state. The small knobs (resolution 4) use raw reads.
 * ======================================================================== */
#define BNK16_BIG_ENCODER 2
static const pin_t   bnk16_enc_pins[2][NUM_ENCODERS] = { ENCODER_A_PINS, ENCODER_B_PINS };
static const int8_t  bnk16_enc_lut[16]               = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 }; /* same as QMK's */
static const uint8_t bnk16_enc_gray[4]               = { 0, 2, 3, 1 }; /* states QMK's LUT counts +1 from one to the next */
static uint8_t       bnk16_big_raw;
static int32_t       bnk16_big_pos;
static int32_t       bnk16_big_filtered;
static int8_t        bnk16_big_dir;

static uint8_t bnk16_big_read_raw(void) {
    return (gpio_read_pin(bnk16_enc_pins[0][BNK16_BIG_ENCODER]) ? 1 : 0) | (gpio_read_pin(bnk16_enc_pins[1][BNK16_BIG_ENCODER]) ? 2 : 0);
}

void encoder_quadrature_init_pin(uint8_t index, bool pad_b) {
    gpio_set_pin_input_high(bnk16_enc_pins[pad_b][index]);
    if (index == BNK16_BIG_ENCODER && pad_b) {
        wait_us(100);
        bnk16_big_raw = bnk16_big_read_raw();
        for (uint8_t i = 0; i < 4; i++) {
            if (bnk16_enc_gray[i] == bnk16_big_raw) {
                bnk16_big_pos = bnk16_big_filtered = i;
            }
        }
    }
}

uint8_t encoder_quadrature_read_pin(uint8_t index, bool pad_b) {
    if (index == BNK16_BIG_ENCODER) {
        uint8_t state = bnk16_enc_gray[bnk16_big_filtered & 3];
        return pad_b ? (state >> 1) & 1 : state & 1;
    }
    return gpio_read_pin(bnk16_enc_pins[pad_b][index]) ? 1 : 0;
}

/* Runs every scan from matrix_task(), before encoder_task() in the same keyboard_task() pass */
void matrix_scan_kb(void) {
    uint8_t state = bnk16_big_read_raw();
    if (state != bnk16_big_raw) {
        bnk16_big_pos += bnk16_enc_lut[(bnk16_big_raw << 2) | state];
        bnk16_big_raw = state;
    }
    if (bnk16_big_pos != bnk16_big_filtered) {
        int8_t dir = bnk16_big_pos > bnk16_big_filtered ? 1 : -1;
        if (dir == bnk16_big_dir || (bnk16_big_pos - bnk16_big_filtered) * dir >= 2) {
            bnk16_big_filtered += dir;
            bnk16_big_dir = dir;
        }
    }
    matrix_scan_user();
}
#endif

int main(void) {
    extern void protocol_setup();
    extern void protocol_pre_init();
    extern void protocol_post_init();
    extern void platform_run();

    platform_setup();

    bnk16_encoder_pins_prepare();

    /* PA11 switches the LED supply (WS2812_EN_PIN), the RGB driver owns it; start with LEDs off */
    GPIOA_ModeCfg(GPIO_Pin_11, GPIO_ModeOut_PP_5mA);
#if WS2812_EN_LEVEL
    GPIOA_ResetBits(GPIO_Pin_11);
#else
    GPIOA_SetBits(GPIO_Pin_11);
#endif

    /* LED指示灯初始化 */
    led_indicator_init();

    protocol_setup();
#if !defined ESB_ENABLE || ESB_ENABLE != 2
    keyboard_setup();
#endif

    protocol_pre_init();
    keyboard_init();
    protocol_post_init();

    /* 初始化 USB 状态记录（只用于后续状态变化检测，不自动切换模式）
     * 必须在 ble_restore_last_mode() 之前设置，避免上电时 housekeeping
     * 检测到"USB 从无到有"而自动切有线，覆盖上电恢复的模式
     */
    last_usb_connected = usb_is_connected();

    /* 初始化电池管理（评估板用模拟值，预留接口）*/
    battery_init();

    /* 初始化电源管理（上电点亮 LED1，启动无操作计时）*/
    power_init();

    /* 上电恢复上次模式（参考量产键盘逻辑）：
     *   上次是有线 → 进入有线模式
     *   上次是蓝牙 → 切换到上次设备槽位，自动回连
     * USB 是否连接不影响恢复结果，模式由用户上次选择决定
     */
    ble_restore_last_mode();

    /* Main loop */
    for (;;) {
        platform_run();
        //! housekeeping_task() is handled by platform

        /* LED指示灯任务 — 处理闪烁和状态超时 */
        led_indicator_task();

        /* WFI（Wait For Interrupt）— CPU 空闲时进入低功耗休眠
         * 任何中断（定时器/矩阵扫描/BLE/USB/GPIO）都会唤醒 CPU
         * 不影响实时性：所有任务都是中断驱动的，唤醒后立即处理
         * 零睡死风险：WFI 只暂停 CPU，所有外设/RAM/寄存器保持状态
         */
        __WFI();
    }
}
