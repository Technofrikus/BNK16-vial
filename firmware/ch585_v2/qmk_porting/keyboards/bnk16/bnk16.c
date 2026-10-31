/*
 * Binepad BNK16 board entry (based on the CH585 EVB entry)
 * Pin map and open points: docs/bnk16/HARDWARE.md
 *
 * Sections: USB and wired mode, layer switch, housekeeping, haptics, RGB, pin setup, big knob, main.
 *
 * Mode logic (the BLE side lives in ble_app.c, not brought up on this board yet):
 *   Power on: restore the last mode; in BLE mode reconnect to the last device
 *   USB plugged in while running: switch to wired, disconnect BLE and stop advertising
 *   USB unplugged while running: no automatic switch to BLE, a BT key has to be pressed
 *   BT_1/2/3, WIRED_MODE, BT_CLEAR: see keymaps/vial/keymap.c
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

#ifdef RGB_MATRIX_ENABLE
static void status_led_task(void);
static void leds_off_task(void);
#endif

void housekeeping_task_kb(void) {
    layer_switch_task();
    bnk16_diag_task();
    power_task();

    /* the BLE stack may enable the RF antenna switch outputs (PB16-PB21) later on; PB21 is a matrix row */
    if (R16_PIN_ALTERNATE & RB_RF_ANT_SW_EN) {
        R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    }

    usb_mode_task();

#ifdef RGB_MATRIX_ENABLE
    status_led_task();
    leds_off_task();
#endif
}

#ifdef RGB_MATRIX_ENABLE
/* ========================================================================
 * RGB matrix: LEDs 0-15 are the keys (row-major, matrix row * 4 + col),
 * LED BNK16_STATUS_LED is the extra status LED. It has no flags, so no
 * effect touches it; it is driven only by the code below.
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

#define BNK16_KEY_LED_COUNT 16

/* Full scale colours, set_led_scaled() applies the Vial brightness */
static const rgb_t COLOR_OFF   = {.r = 0, .g = 0, .b = 0};
static const rgb_t COLOR_WHITE = {.r = 255, .g = 255, .b = 255};
static const rgb_t COLOR_RED   = {.r = 255, .g = 0, .b = 0};
static const rgb_t COLOR_GREEN = {.r = 0, .g = 255, .b = 0};
static const rgb_t COLOR_BLUE  = {.r = 0, .g = 0, .b = 255};

static const rgb_t layer_colors[] = {
    {.r = 255, .g = 255, .b = 255}, /* white */
    {.r = 150, .g = 0, .b = 255},   /* purple */
    {.r = 255, .g = 100, .b = 0},   /* orange */
    {.r = 0, .g = 255, .b = 0},     /* green */
};

#define STATUS_BATTERY_ON_MS    10000 /* on battery the layer colour goes off after this */
#define STATUS_CONNECTED_MS     2000
#define STATUS_FLASH_FAILED_MS  600   /* one period of the red flashes */
#define STATUS_FLASH_FAILED_N   3
#define STATUS_FLASH_MOBILE_MS  400   /* one period of the white flashes */
#define BLINK_PAIRING_MS        500
#define BLINK_SEARCHING_MS      1000
#define LEDS_OFF_REFRESH_MS     50
#define LEDS_OFF_SETTLE_TICKS   10    /* frames sent regardless of changes after QMK blanked the LEDs */

typedef enum {
    STATUS_MSG_NONE,
    STATUS_MSG_FAILED,
    STATUS_MSG_CONNECTED,
    STATUS_MSG_MOBILE,
} status_msg_t;

static status_msg_t status_msg;
static uint8_t      status_msg_flashes;
static uint32_t     status_msg_start;
static uint32_t     status_layer_start;

static void status_msg_show(status_msg_t msg, uint8_t flashes) {
    status_msg         = msg;
    status_msg_flashes = flashes;
    status_msg_start   = timer_read32();
}

/* Called by the mobile mode key: 2 flashes = on, 1 = off */
void bnk16_status_mobile_mode(bool on) {
    status_msg_show(STATUS_MSG_MOBILE, on ? 2 : 1);
}

static inline bool blink_on(uint16_t period_ms) {
    return timer_read32() % period_ms < period_ms / 2;
}

static bool ble_is_pairing(void) {
    return ble_get_state() == BLE_STATE_PAIRING;
}

/* Looking for a host, but not in pairing mode */
static bool ble_is_searching(void) {
    switch (ble_get_state()) {
        case BLE_STATE_ADVERTISING:
        case BLE_STATE_RECONNECTING:
        case BLE_STATE_CONNECTING:
        case BLE_STATE_SWITCHING:
            return true;
        default:
            return false;
    }
}

/* Follows the states that start or end something on the status LED and handles sleep. Timing starts
 * here, the colour itself is chosen in status_led_color(). */
static void status_led_task(void) {
    static uint8_t last_layer    = 0xFF;
    static bool    last_sleeping = false;
    static bool    last_usb      = false;
    static bool    ble_waiting   = false; /* pairing or searching, no result shown yet */

    uint8_t layer    = get_highest_layer(layer_state | default_layer_state);
    bool    sleeping = power_get_state() == POWER_STATE_SLEEPING;
    bool    usb      = usb_is_connected();

    if (sleeping != last_sleeping) {
        rgb_matrix_set_suspend_state(sleeping);
    }
    if (layer != last_layer || usb != last_usb || (last_sleeping && !sleeping)) {
        status_layer_start = timer_read32();
    }
    last_layer    = layer;
    last_sleeping = sleeping;
    last_usb      = usb;

    if (ble_is_pairing() || ble_is_searching()) {
        ble_waiting        = true;
        status_layer_start = timer_read32();
    } else if (ble_waiting) {
        ble_waiting = false;
        if (ble_is_connected()) {
            status_msg_show(STATUS_MSG_CONNECTED, 0);
        } else if (!wired_mode_active) {
            status_msg_show(STATUS_MSG_FAILED, STATUS_FLASH_FAILED_N);
        }
    }

    if (status_msg != STATUS_MSG_NONE) {
        uint32_t duration = STATUS_CONNECTED_MS;
        if (status_msg == STATUS_MSG_FAILED) {
            duration = STATUS_FLASH_FAILED_MS * status_msg_flashes;
        } else if (status_msg == STATUS_MSG_MOBILE) {
            duration = STATUS_FLASH_MOBILE_MS * status_msg_flashes;
        }
        if (timer_elapsed32(status_msg_start) >= duration) {
            status_msg         = STATUS_MSG_NONE;
            status_layer_start = timer_read32();
        }
    }
}

/* Highest priority first: pairing, searching, result and mobile mode messages, layer colour */
static rgb_t status_led_color(void) {
    if (ble_is_pairing()) {
        return blink_on(BLINK_PAIRING_MS) ? COLOR_BLUE : COLOR_OFF;
    }
    if (ble_is_searching()) {
        return blink_on(BLINK_SEARCHING_MS) ? COLOR_BLUE : COLOR_OFF;
    }
    uint32_t elapsed = timer_elapsed32(status_msg_start);
    switch (status_msg) {
        case STATUS_MSG_FAILED:
            return elapsed % STATUS_FLASH_FAILED_MS < STATUS_FLASH_FAILED_MS / 2 ? COLOR_RED : COLOR_OFF;
        case STATUS_MSG_CONNECTED:
            return COLOR_BLUE;
        case STATUS_MSG_MOBILE:
            return elapsed % STATUS_FLASH_MOBILE_MS < STATUS_FLASH_MOBILE_MS / 2 ? COLOR_WHITE : COLOR_OFF;
        default:
            break;
    }
    /* no VBUS pin: a plain charger or a host in standby counts as battery here */
    if (!usb_is_connected() && timer_elapsed32(status_layer_start) >= STATUS_BATTERY_ON_MS) {
        return COLOR_OFF;
    }
    uint8_t layer = get_highest_layer(layer_state | default_layer_state);
    return layer < ARRAY_SIZE(layer_colors) ? layer_colors[layer] : COLOR_OFF;
}

/* The one place for keys with a colour of their own. These stay lit when the LEDs are deactivated in Vial. */
static bool key_color(uint16_t keycode, rgb_t *color) {
    /* the key that toggles the haptic brake shows its state: red = on, green = off */
    if (bnk16_keycode_is_haptic_brake(keycode)) {
        *color = bnk16_haptic_brake_on() ? COLOR_RED : COLOR_GREEN;
        return true;
    }
    /* the knob resolution keys go from green to red as they near the end of their range */
    bool up = bnk16_keycode_is_knob_res(keycode, true);
    if (up || bnk16_keycode_is_knob_res(keycode, false)) {
        uint8_t range = BNK16_BIG_STEPS_PER_KEY_MAX - 1;
        uint8_t left  = up ? bnk16_knob_steps() - 1 : BNK16_BIG_STEPS_PER_KEY_MAX - bnk16_knob_steps();
        color->r      = 255 * (range - left) / range;
        color->g      = 255 * left / range;
        color->b      = 0;
        return true;
    }
    /* the key of the selected Bluetooth device shows its connection */
    uint8_t device = bnk16_keycode_bt_device(keycode);
    if (device != 0) {
        uint8_t current;
        ble_load_mode(NULL, &current);
        if (device == current) {
            if (ble_is_connected()) {
                *color = COLOR_BLUE;
                return true;
            }
            if (ble_is_pairing()) {
                *color = blink_on(BLINK_PAIRING_MS) ? COLOR_BLUE : COLOR_OFF;
                return true;
            }
            if (ble_is_searching()) {
                *color = blink_on(BLINK_SEARCHING_MS) ? COLOR_BLUE : COLOR_OFF;
                return true;
            }
        }
    }
    return false;
}

static inline rgb_t scale_to_brightness(rgb_t color) {
    uint8_t val = rgb_matrix_get_val();
    color.r     = color.r * val / 255;
    color.g     = color.g * val / 255;
    color.b     = color.b * val / 255;
    return color;
}

static inline uint16_t active_keycode_of_led(uint8_t led) {
    keypos_t pos = {.row = led / 4, .col = led % 4};
    return keymap_key_to_keycode(layer_switch_get_layer(pos), pos);
}

/* Runs after the effect wrote its frame: a key's own colour replaces the effect, unassigned keys go
 * dark, all others keep the effect. KC_TRNS resolves to the layer below, so it counts as assigned
 * if that key is. The port's own rgb_matrix_indicators_advanced_kb() (extra_indicator.c) calls this hook. */
bool rgb_matrix_indicators_advanced_user(uint8_t led_min, uint8_t led_max) {
    if (BNK16_STATUS_LED >= led_min && BNK16_STATUS_LED < led_max) {
        rgb_t color = scale_to_brightness(status_led_color());
        rgb_matrix_set_color(BNK16_STATUS_LED, color.r, color.g, color.b);
    }
    for (uint8_t led = led_min; led < led_max && led < BNK16_KEY_LED_COUNT; led++) {
        uint16_t keycode = active_keycode_of_led(led);
        rgb_t    color;
        if (key_color(keycode, &color)) {
            color = scale_to_brightness(color);
            rgb_matrix_set_color(led, color.r, color.g, color.b);
        } else if (keycode == KC_NO && !bnk16_show_unassigned()) {
            rgb_matrix_set_color(led, 0, 0, 0);
        }
    }
    return true;
}

/* QMK skips the indicator hook while the LEDs are deactivated in Vial and only blanks them once, so the
 * special keys and the status LED are drawn here. Frames are only sent on a change. QMK's blank frame
 * can arrive after ours, hence the few unconditional frames at the start. Not while sleeping: QMK keeps
 * everything dark then. */
static void leds_off_task(void) {
    static uint32_t last_time;
    static uint8_t  settle_ticks = LEDS_OFF_SETTLE_TICKS;
    static rgb_t    last[RGB_MATRIX_LED_COUNT];

    if (timer_elapsed32(last_time) < LEDS_OFF_REFRESH_MS) {
        return;
    }
    last_time = timer_read32();

    if (power_get_state() == POWER_STATE_SLEEPING || (rgb_matrix_is_enabled() && rgb_matrix_get_mode() != RGB_MATRIX_NONE)) {
        settle_ticks = LEDS_OFF_SETTLE_TICKS;
        return;
    }

    bool changed = settle_ticks > 0;
    if (settle_ticks > 0) {
        settle_ticks--;
    }
    for (uint8_t led = 0; led < RGB_MATRIX_LED_COUNT; led++) {
        rgb_t color = COLOR_OFF;
        if (led == BNK16_STATUS_LED) {
            color = status_led_color();
        } else if (led < BNK16_KEY_LED_COUNT && !key_color(active_keycode_of_led(led), &color)) {
            color = COLOR_OFF;
        }
        color = scale_to_brightness(color);
        if (color.r != last[led].r || color.g != last[led].g || color.b != last[led].b) {
            last[led] = color;
            changed   = true;
        }
        rgb_matrix_set_color(led, color.r, color.g, color.b);
    }
    if (changed) {
        rgb_matrix_update_pwm_buffers();
    }
}
#endif

/* ========================================================================
 * Haptics: TMR0 steps PA7 through a pattern of half-period slots, the pin rests high (motor off).
 * A burst is N push pulses (low = on) on every second slot: ON off ON off ... ON, so N = 1 is one
 * push of half a period (3.9 ms at 128 Hz), the shortest the motor responds to well.
 * The first push starts at once. The optional brake adds two idle slots and then one push, half a
 * period out of phase with the drive, to counter the ringing. The drive can only push one way, so
 * this damps the motor instead of stopping it.
 * ======================================================================== */
typedef union {
    uint32_t raw;
    struct {
        uint32_t freq_hz : 9;
        uint32_t pulses : 5;
        uint32_t knob_steps : 4; /* big knob steps per key press and burst */
        uint32_t brake : 1;
        uint32_t show_unassigned : 1; /* LED setting kept in this word: 0 = unassigned keys stay dark */
        uint32_t unused : 4;
        uint32_t magic : 8;
    };
} haptic_config_t;

#define HAPTIC_MAGIC 0xA9 /* tells this layout from older stored words */

static haptic_config_t haptic_config;

static volatile uint8_t haptic_slot;
static volatile uint8_t haptic_slots;      /* slots of the whole pattern, incl. brake */
static volatile uint8_t haptic_drive_slots;
static volatile bool    haptic_brake;
static volatile bool    haptic_running = false;

static inline bool haptic_slot_on(uint8_t slot) {
    return (slot < haptic_drive_slots && !(slot & 1)) || (haptic_brake && slot == haptic_drive_slots + 2);
}

static inline void haptic_pin(bool on) {
    if (on) {
        GPIOA_ResetBits(GPIO_Pin_7);
    } else {
        GPIOA_SetBits(GPIO_Pin_7);
    }
}

__INTERRUPT __HIGH_CODE void TMR0_IRQHandler(void) {
    TMR0_ClearITFlag(RB_TMR_IF_CYC_END);
    if (++haptic_slot < haptic_slots) {
        haptic_pin(haptic_slot_on(haptic_slot));
    } else {
        GPIOA_SetBits(GPIO_Pin_7);
        R8_TMR0_INTER_EN = 0;
        R8_TMR0_CTRL_MOD = RB_TMR_ALL_CLEAR;
        haptic_running   = false;
    }
}

/* (Re)starts the pattern; a burst that is still running is cut off and begins again */
static void haptic_buzz(void) {
    PFIC_DisableIRQ(TMR0_IRQn);
    haptic_brake       = haptic_config.brake;
    haptic_drive_slots = 2 * haptic_config.pulses - 1;
    haptic_slots       = haptic_drive_slots + (haptic_brake ? 3 : 0);
    haptic_slot        = 0;
    haptic_running     = true;
    haptic_pin(true);
    TMR0_TimerInit(FREQ_SYS / (2 * haptic_config.freq_hz));
    TMR0_ClearITFlag(RB_TMR_IF_CYC_END);
    TMR0_ITCfg(ENABLE, RB_TMR_IE_CYC_END);
    PFIC_EnableIRQ(TMR0_IRQn);
}

static void haptic_set_defaults(void) {
    haptic_config.raw     = 0;
    haptic_config.magic   = HAPTIC_MAGIC;
    haptic_config.freq_hz = BNK16_HAPTIC_FREQ_HZ;
    haptic_config.pulses  = BNK16_HAPTIC_PULSES;
    haptic_config.knob_steps = BNK16_BIG_STEPS_PER_KEY;
    haptic_config.brake   = BNK16_HAPTIC_BRAKE;
}

/* After a reset, a factory reset or an older stored layout the word is invalid: use the defaults */
static void haptic_load_config(void) {
    haptic_config.raw = eeconfig_read_kb();
    if (haptic_config.magic != HAPTIC_MAGIC || haptic_config.freq_hz < BNK16_HAPTIC_FREQ_MIN || haptic_config.freq_hz > BNK16_HAPTIC_FREQ_MAX || haptic_config.pulses < 1 || haptic_config.pulses > BNK16_HAPTIC_PULSES_MAX || haptic_config.knob_steps < 1 || haptic_config.knob_steps > BNK16_BIG_STEPS_PER_KEY_MAX) {
        haptic_set_defaults();
    }
}

static void haptic_save_and_buzz(void) {
    eeconfig_update_kb(haptic_config.raw);
    haptic_buzz();
}

/* Called by the custom keycodes in keymap.c; every change gives a sample burst */
void bnk16_haptic_reset(void) {
    bool show_unassigned = haptic_config.show_unassigned;
    haptic_set_defaults();
    haptic_config.show_unassigned = show_unassigned;
    haptic_save_and_buzz();
}

void bnk16_toggle_show_unassigned(void) {
    haptic_config.show_unassigned = !haptic_config.show_unassigned;
    eeconfig_update_kb(haptic_config.raw);
}

bool bnk16_show_unassigned(void) {
    return haptic_config.show_unassigned;
}

void bnk16_haptic_toggle_brake(void) {
    haptic_config.brake = !haptic_config.brake;
    haptic_save_and_buzz();
}

void bnk16_haptic_change_freq(bool up) {
    uint16_t freq = haptic_config.freq_hz;
    if (up ? freq + BNK16_HAPTIC_FREQ_STEP <= BNK16_HAPTIC_FREQ_MAX : freq - BNK16_HAPTIC_FREQ_STEP >= BNK16_HAPTIC_FREQ_MIN) {
        haptic_config.freq_hz = up ? freq + BNK16_HAPTIC_FREQ_STEP : freq - BNK16_HAPTIC_FREQ_STEP;
    }
    haptic_save_and_buzz();
}

void bnk16_haptic_change_length(bool up) {
    if (up ? haptic_config.pulses < BNK16_HAPTIC_PULSES_MAX : haptic_config.pulses > 1) {
        haptic_config.pulses = up ? haptic_config.pulses + 1 : haptic_config.pulses - 1;
    }
    haptic_save_and_buzz();
}

/* Higher resolution = fewer steps per key press */
void bnk16_knob_change_resolution(bool up) {
    if (up ? haptic_config.knob_steps > 1 : haptic_config.knob_steps < BNK16_BIG_STEPS_PER_KEY_MAX) {
        haptic_config.knob_steps = up ? haptic_config.knob_steps - 1 : haptic_config.knob_steps + 1;
    }
    haptic_save_and_buzz();
}

bool bnk16_haptic_brake_on(void) {
    return haptic_config.brake;
}

uint8_t bnk16_knob_steps(void) {
    return haptic_config.knob_steps;
}

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
    /* PA7 = haptic P-FET gate, high = motor off; set the level first so it never starts on */
    GPIOA_SetBits(GPIO_Pin_7);
    GPIOA_ModeCfg(GPIO_Pin_7, GPIO_ModeOut_PP_5mA);
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
static const pin_t   bnk16_enc_pins[2][NUM_ENCODERS] = { ENCODER_A_PINS, ENCODER_B_PINS };
static const int8_t  bnk16_enc_lut[16]               = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 }; /* same as QMK's */
static const uint8_t bnk16_enc_gray[4]               = { 0, 2, 3, 1 }; /* states QMK's LUT counts +1 from one to the next */
static volatile uint8_t bnk16_big_raw;
static volatile int32_t bnk16_big_pos;
static volatile int32_t bnk16_big_filtered;
static volatile int8_t  bnk16_big_dir;
static volatile int32_t bnk16_big_steps; /* filtered steps not yet handed to QMK, counted by the interrupt */

static inline __attribute__((always_inline)) uint8_t bnk16_big_read_raw(void) {
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
        /* sample the knob at 4 kHz in an interrupt, see TMR2_IRQHandler */
        TMR2_TimerInit(FREQ_SYS / 4000);
        TMR2_ClearITFlag(RB_TMR_IF_CYC_END);
        TMR2_ITCfg(ENABLE, RB_TMR_IE_CYC_END);
        PFIC_EnableIRQ(TMR2_IRQn);
    }
}

uint8_t encoder_quadrature_read_pin(uint8_t index, bool pad_b) {
    if (index == BNK16_BIG_ENCODER) {
        return 0; /* constant: QMK's decoder never sees the big knob, its steps are queued in matrix_scan_kb */
    }
    return gpio_read_pin(bnk16_enc_pins[pad_b][index]) ? 1 : 0;
}

/* in QMK's encoder.c, but not declared in its header */
extern bool encoder_queue_full(void);

/* 4 kHz sampling of the big knob. The main loop is blocked for milliseconds at a time while key reports
 * are sent, which used to lose quadrature states on fast turns (and show up as steps in the wrong
 * direction). The interrupt tracks every state change and counts the filtered steps. */
__INTERRUPT __HIGH_CODE void TMR2_IRQHandler(void) {
    TMR2_ClearITFlag(RB_TMR_IF_CYC_END);
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
            bnk16_big_steps += dir;
        }
    }
}

/* One key press and one haptic burst per N steps; a change of direction restarts the count. Steps that
 * come too soon after the last key or find the queue full are dropped without a burst, so a click always
 * means a key was sent and no backlog can build up. */
static void big_knob_step(bool positive) {
    static uint8_t  step_count;
    static bool     last_positive;
    static uint32_t last_key_time;

    if (positive != last_positive) {
        last_positive = positive;
        step_count    = 0;
    }
    if (++step_count < haptic_config.knob_steps) {
        return;
    }
    step_count = 0;
    if (timer_elapsed32(last_key_time) >= BNK16_BIG_MIN_KEY_INTERVAL_MS && !encoder_queue_full()) {
        last_key_time = timer_read32();
        encoder_queue_event(BNK16_BIG_ENCODER, !positive);
        haptic_buzz();
    }
}

/* Runs every scan from matrix_task(), before encoder_task() in the same keyboard_task() pass: hands the
 * steps counted by the interrupt to big_knob_step (a positive step is what QMK's own decoder reports as
 * ENCODER_COUNTER_CLOCKWISE, i.e. false). */
void matrix_scan_kb(void) {
    /* no atomic instruction here: the core may not run amoswap, so take the count with the interrupt masked */
    PFIC_DisableIRQ(TMR2_IRQn);
    int32_t n        = bnk16_big_steps;
    bnk16_big_steps = 0;
    PFIC_EnableIRQ(TMR2_IRQn);
    while (n != 0) {
        bool positive = n > 0;
        n += positive ? -1 : 1;
        big_knob_step(positive);
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
    haptic_load_config();
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
