/*
 * Binepad BNK16 board entry (based on the CH585 EVB entry)
 * Pin map and open points: docs/bnk16/HARDWARE.md
 *
 * Sections: USB and wired mode, layer switch, housekeeping, RGB, pin setup, big knob, main.
 *
 * Mode logic (the BLE side lives in ble_app.c, not brought up on this board yet):
 *   Power on: restore the last mode; in BLE mode reconnect to the last device
 *   USB plugged in while running: switch to wired, disconnect BLE and stop advertising
 *   USB unplugged while running: no automatic switch to BLE, a BT key has to be pressed
 *   BT_1/2/3, WIRED_MODE, BT_CLEAR: see keymaps/default/keymap.c
 */

#include QMK_KEYBOARD_H
#include "ble_app.h"
#include "power_management.h"
#include "battery.h"
#include "bnk16_diag.h"
#include "CH58x_common.h"

/* ========================================================================
 * USB and wired mode
 * ======================================================================== */

/* Read by the state callbacks in ble_app.c */
bool wired_mode_active = false;

static bool last_usb_connected = false;

/* USB connected and enumerated. The port never reports to QMK's usb_device_state (it always reads
 * INIT), so read the USBHS driver state directly: EnumStatus is set by SET_CONFIGURATION and cleared
 * by a bus reset; bit 1 of SleepStatus is the bus suspend, which is also what an unplugged cable looks
 * like (no VBUS pin is known). A host in standby therefore also counts as not connected. */
extern volatile uint8_t USBHS_DevEnumStatus;
extern volatile uint8_t USBHS_DevSleepStatus;
#define USBHS_SLEEP_SUSPENDED 0x02

static bool usb_is_connected(void) {
    return USBHS_DevEnumStatus && !(USBHS_DevSleepStatus & USBHS_SLEEP_SUSPENDED);
}

void enter_wired_mode(void) {
    /* set first: the BLE state callbacks triggered by the disconnect below check it */
    wired_mode_active = true;
    power_set_wired_mode(true); /* no sleep in wired mode */
    ble_save_mode(true, 1);
    if (ble_is_connected()) {
        ble_disconnect();
    }
    ble_stop_advertising();
}

static void exit_wired_mode(void) {
    wired_mode_active = false;
    power_set_wired_mode(false);
}

static void usb_mode_task(void) {
    bool usb_connected = usb_is_connected();

    if (usb_connected != last_usb_connected) {
        if (usb_connected) {
            enter_wired_mode();
        } else {
            exit_wired_mode();
        }
        last_usb_connected = usb_connected;
    }

    /* the BLE stack can restart advertising on its own */
    if (wired_mode_active && (ble_get_state() == BLE_STATE_ADVERTISING || ble_get_state() == BLE_STATE_PAIRING)) {
        ble_stop_advertising();
    }
}

/* ========================================================================
 * Layer switch
 * ======================================================================== */

/* 4-position layer switch: its two contacts are matrix (0,4) and (1,4) (column PB10, diodes to
 * rows PB3/PB2), measured: none = layer 0, (0,4) = 1, (1,4) = 2, both = 3. They are KC_NO in LAYOUT
 * and hidden in vial.json. The layer is only moved when the switch position changes, so MO()/TG()
 * keys keep working on top of it. The matrix is debounced, so the value is stable here. */
#define LAYER_SWITCH_SETTLE_MS 100 /* first matrix scans after boot are not valid yet */

static void layer_switch_task(void) {
    static uint8_t last = 0xFF;
    uint8_t        pos  = (matrix_is_on(0, 4) ? 1 : 0) | (matrix_is_on(1, 4) ? 2 : 0);

    if (pos != last && timer_read32() > LAYER_SWITCH_SETTLE_MS) {
        last = pos;
        layer_move(pos);
    }
}

/* ========================================================================
 * Housekeeping (called from QMK's main loop)
 * ======================================================================== */

void housekeeping_task_kb(void) {
    layer_switch_task();
    bnk16_diag_task();
    power_task();

    /* the BLE stack may enable the RF antenna switch outputs (PB16-PB21) later on; PB21 is a matrix row */
    if (R16_PIN_ALTERNATE & RB_RF_ANT_SW_EN) {
        R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    }

    usb_mode_task();
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

/* ========================================================================
 * Pin setup, runs once before QMK configures the pins
 * ======================================================================== */

/* PB14/PB15 are also the TIO/TCK debug pins, and PB16-PB23 have separate digital-input-disable bits
 * (R16_PIN_CONFIG) next to the PB0-PB15 ones (R32_PIN_IN_DIS). Make sure every encoder pin is a plain
 * digital input before QMK's encoder driver configures the pull-ups. */
static void bnk16_pins_prepare(void) {
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

    bnk16_pins_prepare();

    /* PA11 switches the LED supply (WS2812_EN_PIN), the RGB driver owns it; start with LEDs off */
    GPIOA_ModeCfg(GPIO_Pin_11, GPIO_ModeOut_PP_5mA);
#if WS2812_EN_LEVEL
    GPIOA_ResetBits(GPIO_Pin_11);
#else
    GPIOA_SetBits(GPIO_Pin_11);
#endif

    protocol_setup();
#if !defined ESB_ENABLE || ESB_ENABLE != 2
    keyboard_setup();
#endif

    protocol_pre_init();
    keyboard_init();
    protocol_post_init();

    /* set before the mode restore, so housekeeping does not see a plug-in and override the restored mode */
    last_usb_connected = usb_is_connected();

    battery_init();
    power_init();
    ble_restore_last_mode();

    for (;;) {
        platform_run();
        //! housekeeping_task() is handled by platform

        /* sleep until the next interrupt; peripherals and RAM keep running */
        __WFI();
    }
}
