/*
 * Binepad BNK16 board entry (based on the CH585 EVB entry)
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
#include "CH58x_common.h"
#include <string.h>
#include <stdio.h>

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

#ifdef BNK16_PIN_SCAN
/* ========================================================================
 * Diagnostic build only: find the pin that switches the LED supply.
 * Starts automatically 20 s after boot. Every candidate pin is driven
 * high for 3 s, then low for 5 s, and its name is typed over USB in lower case without Shift or Enter ("a3h ", "b12l ").
 * Excluded: matrix, encoders, PA10 (LED data), PB8 (LED enable), USB, PB22.
 * ======================================================================== */
static const struct { pin_t pin; const char *name; } scan_pins[] = {
    { A0, "a0" },
    { A1, "a1" },
    { A2, "a2" },
    { A3, "a3" },
    { A4, "a4" },
    { A5, "a5" },
    { A6, "a6" },
    { A7, "a7" },
    { A8, "a8" },
    { A9, "a9" },
    { A11, "a11" },
    { A12, "a12" },
    { A13, "a13" },
    { A14, "a14" },
    { A15, "a15" },
    { B9, "b9" },
    { B12, "b12" },
    { B13, "b13" },
    { B18, "b18" },
    { B19, "b19" },
    { B23, "b23" }
};
#define SCAN_N (sizeof(scan_pins) / sizeof(scan_pins[0]))
#define SCAN_STEP_MS 5000

static int8_t   scan_idx   = -1;
static uint8_t  scan_phase = 0;
static uint32_t scan_t     = 0;

/* Type without Enter and release everything afterwards, so no key can stay held and auto-repeat. */
static void scan_type(const char *str) {
    send_string_with_delay(str, 30);
    wait_ms(30);
    clear_keyboard();
    wait_ms(30);
    clear_keyboard();
}

static void scan_begin_phase(void) {
    gpio_set_pin_output(scan_pins[scan_idx].pin);
    gpio_write_pin(scan_pins[scan_idx].pin, scan_phase == 0);
    scan_type(scan_pins[scan_idx].name);
    scan_type(scan_phase == 0 ? "h " : "l ");
    scan_t = timer_read32();
}

static void pin_scan_task(void) {
    static bool started = false;

    /* Starts by itself 20 s after boot (time to focus a text field); no key is involved,
     * so no physical key can be held while the report is rewritten. */
    if (!started && timer_read32() > 20000) {
        started    = true;
        scan_idx   = 0;
        scan_phase = 0;
        scan_type("start ");
        scan_begin_phase();
    }

    if (scan_idx < 0 || timer_elapsed32(scan_t) < SCAN_STEP_MS) {
        return;
    }
    if (scan_phase == 0) {
        scan_phase = 1;
    } else {
        gpio_set_pin_input(scan_pins[scan_idx].pin);
        scan_phase = 0;
        if (++scan_idx >= (int8_t)SCAN_N) {
            scan_idx = -1;
            scan_type("end ");
            return;
        }
    }
    scan_begin_phase();
}
#endif

#ifdef BNK16_ENCODER_DEBUG
static void click_probe_task(void);
#endif
#ifdef BNK16_SWITCH_PROBE
static void switch_probe_task(void);
#endif

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
#ifdef BNK16_PIN_SCAN
    pin_scan_task();
#endif
#ifdef BNK16_SWITCH_PROBE
    switch_probe_task();
#endif
#ifdef BNK16_ENCODER_DEBUG
    click_probe_task();
#endif
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
        if (!gpio_read_pin(B11)) {
            /* temporary: BT/USB slide switch (PB11, pull-up) reads low -> white, to find its polarity */
            rgb_matrix_set_color(BNK16_STATUS_LED, 30, 30, 30);
        } else if (usb_is_connected()) {
            rgb_matrix_set_color(BNK16_STATUS_LED, 0, 40, 0);
        } else if (ble_is_connected()) {
            rgb_matrix_set_color(BNK16_STATUS_LED, 0, 0, 40);
        } else {
            rgb_matrix_set_color(BNK16_STATUS_LED, 40, 16, 0);
        }
    }
#ifdef BNK16_ENCODER_DEBUG
    /* Raw encoder pin levels on the top two key rows (green = high, red = low):
     * LED 0/1 = left A/B (PB14/PB15), LED 2/3 = right A/B (PB17/PB16), LED 4/5 = big A/B (PB8/PB9).
     * Turning a knob must make its pair flicker; a pair that never changes points at the pins. */
    static const pin_t enc_debug_pins[] = { B14, B15, B17, B16, B8, B9 };
    for (uint8_t i = 0; i < sizeof(enc_debug_pins) / sizeof(enc_debug_pins[0]); i++) {
        if (i >= led_min && i < led_max) {
            if (gpio_read_pin(enc_debug_pins[i])) {
                rgb_matrix_set_color(i, 0, 60, 0);
            } else {
                rgb_matrix_set_color(i, 60, 0, 0);
            }
        }
    }
    /* LED 8/9 = matrix (4,0)/(4,1) as QMK sees them (green = pressed), LED 10 = PB21 level between scans
     * (red = something pulls PB21 low by itself, e.g. a click wired to GND instead of a column) */
    if (8 >= led_min && 8 < led_max) rgb_matrix_set_color(8, matrix_is_on(4, 0) ? 0 : 20, matrix_is_on(4, 0) ? 60 : 0, 0);
    if (9 >= led_min && 9 < led_max) rgb_matrix_set_color(9, matrix_is_on(4, 1) ? 0 : 20, matrix_is_on(4, 1) ? 60 : 0, 0);
    if (10 >= led_min && 10 < led_max) rgb_matrix_set_color(10, gpio_read_pin(B21) ? 0 : 60, gpio_read_pin(B21) ? 60 : 0, 0);
#endif
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
    /* PB11 = BT/USB slide switch (traced on the PCB; the switch also cuts battery +). Polarity unknown yet. */
    gpio_set_pin_input_high(B11);
}

#ifdef BNK16_ENCODER_DEBUG
/* ========================================================================
 * Diagnostic: find the pin the encoder clicks really connect to.
 * Every 100 ms, column 0 and 1 are each tested against every free pin, in both
 * directions (column low -> read pin, pin low -> read column), so the diode
 * direction does not matter. Hits are typed over USB, e.g. "c0>b20 " (column 0
 * pulled b20 low) or "b20>c0 " (b20 pulled column 0 low); "none " on release.
 * Excluded: matrix rows/cols, encoders, LED data/supply, USB, PB23 (reset).
 * ======================================================================== */
static const pin_t bnk16_col_pins[MATRIX_COLS] = MATRIX_COL_PINS;
static const struct { pin_t pin; const char *name; } click_probe_pins[] = {
    { A0, "a0" }, { A1, "a1" }, { A2, "a2" }, { A3, "a3" }, { A4, "a4" }, { A5, "a5" }, { A6, "a6" },
    { A7, "a7" }, { A8, "a8" }, { A9, "a9" }, { A12, "a12" }, { A13, "a13" }, { A14, "a14" }, { A15, "a15" },
    { B18, "b18" }, { B19, "b19" }, { B20, "b20" }, { B22, "b22" },
};

static void click_probe_task(void) {
    static uint32_t last = 0;
    static char     reported[64] = "";
    char            found[64]    = "";

    if (timer_elapsed32(last) < 100) {
        return;
    }
    last = timer_read32();

    for (uint8_t c = 0; c < 2; c++) {
        pin_t col = bnk16_col_pins[c];
        for (uint8_t i = 0; i < sizeof(click_probe_pins) / sizeof(click_probe_pins[0]); i++) {
            pin_t pin = click_probe_pins[i].pin;
            bool  col_drives, pin_drives;

            gpio_set_pin_input_high(pin);
            gpio_set_pin_output(col);
            gpio_write_pin_low(col);
            wait_us(30);
            col_drives = !gpio_read_pin(pin);
            gpio_set_pin_input_high(col);
            wait_us(30);

            gpio_set_pin_output(pin);
            gpio_write_pin_low(pin);
            wait_us(30);
            pin_drives = !gpio_read_pin(col);
            gpio_set_pin_input_high(pin);
            wait_us(30);

            if ((col_drives || pin_drives) && strlen(found) + 12 < sizeof(found)) {
                char entry[16];
                if (col_drives) {
                    snprintf(entry, sizeof(entry), "c%u>%s ", c, click_probe_pins[i].name);
                } else {
                    snprintf(entry, sizeof(entry), "%s>c%u ", click_probe_pins[i].name, c);
                }
                strcat(found, entry);
            }
        }
    }

    if (strcmp(found, reported) != 0) {
        strcpy(reported, found);
        send_string(found[0] ? found : "none ");
        clear_keyboard();
    }
}
#endif

#ifdef BNK16_SWITCH_PROBE
/* ========================================================================
 * Diagnostic: find the pins of the 4-position layer switch, the BLE/wired
 * switch and the haptics switch. Every 50 ms all free pins are checked:
 *   "a5l"     a5 reads low with its pull-up       (switched to GND)
 *   "a5h"     a5 reads high with its pull-down    (switched to VCC)
 *   "a5xb19"  driving one pin low pulls the other (switch between two pins,
 *             also against matrix rows/cols and encoder pins, any diode direction)
 *   "m42"     QMK matrix position (4,2) is on     (switch inside the matrix)
 *   "a6v78"   ADC value of a6 with its pull-up, 0-99 % of the ADC range (for a
 *             resistor ladder that never crosses the digital threshold); shown
 *             for pins tied high and for pins that moved away from their value
 *             at start, with 4 % hysteresis
 * Free pins are driven only when they are neither tied high nor low. Once the
 * result has been stable for 200 ms and differs from the last line, it is typed
 * over USB as one line: running number, then the tokens ("leer" if nothing).
 * Starts 5 s after boot with "start". Only lower case, digits, space, Enter.
 * Excluded: PA10/PA11 (LEDs), PB10-PB13 (USB FS/HS), PB23 (reset).
 * ======================================================================== */
static const struct { pin_t pin; const char *name; } sp_pins[] = {
    /* free pins: pull-up, pull-down, driven */
    { A0, "a0" }, { A1, "a1" }, { A2, "a2" }, { A3, "a3" }, { A4, "a4" }, { A5, "a5" }, { A6, "a6" },
    { A7, "a7" }, { A8, "a8" }, { A9, "a9" }, { A12, "a12" }, { A13, "a13" }, { A14, "a14" }, { A15, "a15" },
    { B18, "b18" }, { B19, "b19" }, { B20, "b20" }, { B22, "b22" },
    /* matrix rows: driven low like QMK's scan does */
    { B3, "b3" }, { B2, "b2" }, { B0, "b0" }, { B1, "b1" }, { B21, "b21" },
    /* matrix cols and encoder pins: only read */
    { B5, "b5" }, { B4, "b4" }, { B6, "b6" }, { B7, "b7" },
    { B14, "b14" }, { B15, "b15" }, { B17, "b17" }, { B16, "b16" }, { B8, "b8" }, { B9, "b9" },
};
#define SP_N       (sizeof(sp_pins) / sizeof(sp_pins[0]))
#define SP_FREE    18
#define SP_DRIVERS (SP_FREE + 5)
#define SP_ADC     14 /* the first 14 free pins (PA0-PA9, PA12-PA15) are ADC inputs */
static const uint8_t sp_adc_channel[SP_ADC] = { 9, 8, 7, 6, 0, 1, 10, 11, 12, 13, 2, 3, 4, 5 };
static int8_t        sp_adc_start[SP_ADC];
static int8_t        sp_adc_shown[SP_ADC];

/* 0-99 % of the ADC range, pin keeps its pull-up */
static int8_t switch_probe_adc(uint8_t i) {
    uint32_t sum = 0;
    ADC_ChannelCfg(sp_adc_channel[i]);
    ADC_ExcutSingleConver();
    for (uint8_t n = 0; n < 4; n++) {
        sum += ADC_ExcutSingleConver();
    }
    return (int8_t)(sum * 100 / (4 * 4096));
}

static void switch_probe_sweep(char *buf, size_t size) {
    uint32_t hi = 0, lo = 0;
    uint64_t up = 0;
    uint64_t pairs[SP_N];
    size_t   len = 0;

    buf[0] = '\0';
    memset(pairs, 0, sizeof(pairs));

    for (uint8_t i = 0; i < SP_FREE; i++) {
        gpio_set_pin_input_low(sp_pins[i].pin);
    }
    wait_us(50);
    for (uint8_t i = 0; i < SP_FREE; i++) {
        if (gpio_read_pin(sp_pins[i].pin)) hi |= 1UL << i;
        gpio_set_pin_input_high(sp_pins[i].pin);
    }
    wait_us(50);
    for (uint8_t i = 0; i < SP_N; i++) {
        if (gpio_read_pin(sp_pins[i].pin)) {
            up |= 1ULL << i;
        } else if (i < SP_FREE) {
            lo |= 1UL << i;
        }
    }

    for (uint8_t d = 0; d < SP_DRIVERS; d++) {
        if (!(up & (1ULL << d)) || (d < SP_FREE && (hi & (1UL << d)))) {
            continue;
        }
        gpio_set_pin_output(sp_pins[d].pin);
        gpio_write_pin_low(sp_pins[d].pin);
        wait_us(30);
        for (uint8_t t = 0; t < SP_N; t++) {
            if (t == d || !(up & (1ULL << t)) || (d >= SP_FREE && t >= SP_FREE)) {
                continue;
            }
            if (!gpio_read_pin(sp_pins[t].pin)) {
                pairs[d < t ? d : t] |= 1ULL << (d < t ? t : d);
            }
        }
        gpio_set_pin_input_high(sp_pins[d].pin);
        wait_us(30);
    }

    for (uint8_t i = 0; i < SP_FREE && len < size; i++) {
        if (lo & (1UL << i)) len += snprintf(buf + len, size - len, "%sl ", sp_pins[i].name);
        if (hi & (1UL << i) && len < size) len += snprintf(buf + len, size - len, "%sh ", sp_pins[i].name);
    }
    for (uint8_t i = 0; i < SP_N && len < size; i++) {
        for (uint8_t j = i + 1; j < SP_N && len < size; j++) {
            if (pairs[i] & (1ULL << j)) len += snprintf(buf + len, size - len, "%sx%s ", sp_pins[i].name, sp_pins[j].name);
        }
    }
    for (uint8_t r = 0; r < MATRIX_ROWS && len < size; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS && len < size; c++) {
            if (matrix_is_on(r, c)) len += snprintf(buf + len, size - len, "m%u%u ", r, c);
        }
    }

    ADC_ExtSingleChSampInit(SampleFreq_4_or_2, ADC_PGA_1_4);
    for (uint8_t i = 0; i < SP_ADC; i++) {
        int8_t v = switch_probe_adc(i);
        if (sp_adc_start[i] < 0) {
            sp_adc_start[i] = sp_adc_shown[i] = v;
        } else if (v - sp_adc_shown[i] >= 4 || sp_adc_shown[i] - v >= 4) {
            sp_adc_shown[i] = v;
        }
        if (len < size && ((hi & (1UL << i)) || sp_adc_shown[i] != sp_adc_start[i])) {
            len += snprintf(buf + len, size - len, "%sv%d ", sp_pins[i].name, sp_adc_shown[i]);
        }
    }
    R8_ADC_CFG &= ~(RB_ADC_BUF_EN | RB_ADC_POWER_ON);
}

static void switch_probe_task(void) {
    static bool     started  = false;
    static uint32_t last     = 0;
    static uint8_t  stable   = 0;
    static uint16_t count    = 0;
    static char     previous[200] = "";
    static char     reported[200] = "";
    char            current[200];
    char            line[220];

    if (!started) {
        if (timer_read32() < 5000) {
            return;
        }
        started = true;
        GPIOADigitalCfg(ENABLE, GPIO_Pin_All & 0xFFFF);
        GPIOBDigitalCfg(ENABLE, GPIO_Pin_18 | GPIO_Pin_19 | GPIO_Pin_20 | GPIO_Pin_22);
        for (uint8_t i = 0; i < SP_FREE; i++) {
            gpio_set_pin_input_high(sp_pins[i].pin);
        }
        memset(sp_adc_start, -1, sizeof(sp_adc_start));
        send_string("start\n");
        strcpy(reported, "-");
    }

    if (timer_elapsed32(last) < 50) {
        return;
    }
    last = timer_read32();

    switch_probe_sweep(current, sizeof(current));
    if (strcmp(current, previous) != 0) {
        strcpy(previous, current);
        stable = 0;
        return;
    }
    if (stable < 4 && ++stable == 4 && strcmp(current, reported) != 0) {
        strcpy(reported, current);
        snprintf(line, sizeof(line), "%u  %s\n", ++count, current[0] ? current : "leer");
        send_string(line);
        clear_keyboard();
    }
}
#endif

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
