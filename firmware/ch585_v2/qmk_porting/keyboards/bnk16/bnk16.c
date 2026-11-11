/*
 * Binepad BNK16 board entry (based on the CH585 EVB entry)
 * Pin map and open points: docs/bnk16/HARDWARE.md
 *
 * Sections: mode switch and USB state, layer switch, housekeeping, RGB, haptics, pin setup, big knob,
 * deep sleep and shutdown, main. The diagnostics of the test builds are in bnk16_sleep_diag.c.
 *
 * Mode logic (the BLE side lives in ble_app.c): the PB11 slide switch is the only authority.
 *   BT position (low): BLE runs, keys go over BLE, even with a USB cable in. Vial over USB keeps working.
 *   USB position (high): keys go over USB, BLE is disconnected and stopped.
 *   Read at boot and debounced at runtime, so moving the switch with a cable in works without a reset.
 *   Going to BT, at boot or at runtime: reconnect to the saved slot, a slot without a bond opens for pairing.
 *   USB enumeration and suspend do not change the mode, usb_is_connected() only serves the status LED.
 *   BT_1/2/3: see keymaps/vial/keymap.c. They do nothing in the USB position.
 */

#include QMK_KEYBOARD_H
#include "ble_app.h"
#include "power_management.h"
#include "battery.h"
#include "bnk16_diag.h"
#include "bnk16_sleep_diag.h"
#include "ws2812_supplement.h"
#include "CH58x_common.h"
#ifdef BNK16_DEEP_SLEEP
#include "ch585_usbhs_device.h"
#endif
#include "bnk16_fault.h" /* BNK16_STAGE(), empty in a normal build */

/* ========================================================================
 * Mode switch and USB state
 * ======================================================================== */

/* Read by the state callbacks in ble_app.c, the send path in protocol_usb.c and keymap.c */
bool wired_mode_active = false;

/* PB11 slide switch: low = BT position, high = USB position (measured) */
#define MODE_SWITCH_DEBOUNCE_MS 50

static inline bool mode_switch_is_usb(void) {
    return gpio_read_pin(B11);
}

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

#ifdef BNK16_SLEEP_DIAG
bool bnk16_usb_is_connected(void) { /* for bnk16_sleep_diag.c */
    return usb_is_connected();
}
#endif

static void enter_wired_mode(void) {
    /* set first: the BLE state callbacks triggered by the disconnect below check it */
    wired_mode_active = true;
    power_set_wired_mode(true); /* no sleep in wired mode */
    ble_radio_off();
}

static void exit_wired_mode(void) {
    wired_mode_active = false;
    power_set_wired_mode(false);
    ble_switch_device(ble_get_current_device());
}

/* The switch must be stable for MODE_SWITCH_DEBOUNCE_MS before the mode follows it */
static void mode_switch_task(void) {
    static bool     last_read;
    static uint32_t last_change;
    bool            usb_position = mode_switch_is_usb();

    if (usb_position != last_read) {
        last_read   = usb_position;
        last_change = timer_read32();
    } else if (usb_position != wired_mode_active && timer_elapsed32(last_change) >= MODE_SWITCH_DEBOUNCE_MS) {
        if (usb_position) {
            enter_wired_mode();
        } else {
            exit_wired_mode();
        }
    }
}

/* ========================================================================
 * Layer switch
 * ======================================================================== */

/* 4-position layer switch: its two contacts are matrix (0,4) and (1,4) (column PB10, diodes to
 * rows PB3/PB2), measured: none = layer 0, (0,4) = 1, (1,4) = 2, both = 3. They are KC_NO in LAYOUT
 * and hidden in vial.json. The layer is only moved when the switch position changes, so MO()/TG()
 * keys keep working on top of it. The matrix is debounced, so the value is stable here.
 * Called from matrix_scan_kb(), which runs after the debounce and before QMK turns the changes of that
 * scan into key events: a key that arrives in the same scan as a new switch position is already handled
 * on the new layer (the first key after the layer switch was moved during the deep sleep). */
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
    bnk16_sleep_diag_task(); /* test builds only */
    bnk16_diag_task();
    power_task();

    /* the BLE stack may enable the RF antenna switch outputs (PB16-PB21) later on; PB21 is a matrix row */
    if (R16_PIN_ALTERNATE & RB_RF_ANT_SW_EN) {
        R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    }

    mode_switch_task();
    ble_app_poll();

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
#define BLINK_PAIRING_MS        500
#define BLINK_SEARCHING_MS      1000
#define LEDS_OFF_REFRESH_MS     50
#define LEDS_OFF_SETTLE_TICKS   10    /* frames sent regardless of changes after QMK blanked the LEDs */

typedef enum {
    STATUS_MSG_NONE,
    STATUS_MSG_FAILED,
    STATUS_MSG_CONNECTED,
} status_msg_t;

static status_msg_t status_msg;
static uint32_t     status_msg_start;
static uint32_t     status_layer_start;

static void status_msg_show(status_msg_t msg) {
    status_msg       = msg;
    status_msg_start = timer_read32();
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
        ws2812_power_block(sleeping); /* black LEDs still draw current, so cut their supply */
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
            status_msg_show(STATUS_MSG_CONNECTED);
        } else if (!wired_mode_active) {
            status_msg_show(STATUS_MSG_FAILED);
        }
    }

    if (status_msg != STATUS_MSG_NONE) {
        uint32_t duration = status_msg == STATUS_MSG_FAILED ? STATUS_FLASH_FAILED_MS * STATUS_FLASH_FAILED_N : STATUS_CONNECTED_MS;
        if (timer_elapsed32(status_msg_start) >= duration) {
            status_msg         = STATUS_MSG_NONE;
            status_layer_start = timer_read32();
        }
    }
}

/* Blue blinking while BLE looks for a host: fast = pairing, slow = searching. False if it does not. */
static bool ble_waiting_color(rgb_t *color) {
    if (ble_is_pairing() || ble_is_searching()) {
        *color = blink_on(ble_is_pairing() ? BLINK_PAIRING_MS : BLINK_SEARCHING_MS) ? COLOR_BLUE : COLOR_OFF;
        return true;
    }
    return false;
}


/* Highest priority first: pairing, searching, result messages, layer colour */
static rgb_t status_led_color(void) {
    rgb_t color;
    if (bnk16_sleep_diag_boot_color(&color)) { /* test builds only */
        return color;
    }
    if (ble_waiting_color(&color)) {
        return color;
    }
    uint32_t elapsed = timer_elapsed32(status_msg_start);
    switch (status_msg) {
        case STATUS_MSG_FAILED:
            return elapsed % STATUS_FLASH_FAILED_MS < STATUS_FLASH_FAILED_MS / 2 ? COLOR_RED : COLOR_OFF;
        case STATUS_MSG_CONNECTED:
            return COLOR_BLUE;
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
    if (device != 0 && device == ble_get_current_device()) {
        if (ble_is_connected()) {
            *color = COLOR_BLUE;
            return true;
        }
        return ble_waiting_color(color);
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
        } else if (led < BNK16_KEY_LED_COUNT) {
            key_color(active_keycode_of_led(led), &color); /* leaves it off for any other key */
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
/* The board's settings word in EEPROM (eeconfig_read_kb): haptics, big knob resolution and the LED mask */
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
} kb_config_t;

#define KB_CONFIG_MAGIC 0xA9 /* tells this layout from older stored words */

static kb_config_t kb_config;

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
    haptic_brake       = kb_config.brake;
    haptic_drive_slots = 2 * kb_config.pulses - 1;
    haptic_slots       = haptic_drive_slots + (haptic_brake ? 3 : 0);
    haptic_slot        = 0;
    haptic_running     = true;
    haptic_pin(true);
    TMR0_TimerInit(FREQ_SYS / (2 * kb_config.freq_hz));
    TMR0_ClearITFlag(RB_TMR_IF_CYC_END);
    TMR0_ITCfg(ENABLE, RB_TMR_IE_CYC_END);
    PFIC_EnableIRQ(TMR0_IRQn);
}

static void kb_config_set_defaults(void) {
    kb_config.raw     = 0;
    kb_config.magic   = KB_CONFIG_MAGIC;
    kb_config.freq_hz = BNK16_HAPTIC_FREQ_HZ;
    kb_config.pulses  = BNK16_HAPTIC_PULSES;
    kb_config.knob_steps = BNK16_BIG_STEPS_PER_KEY;
    kb_config.brake   = BNK16_HAPTIC_BRAKE;
}

/* After a reset, a factory reset or an older stored layout the word is invalid: use the defaults */
static void kb_config_load(void) {
    kb_config.raw = eeconfig_read_kb();
    if (kb_config.magic != KB_CONFIG_MAGIC || kb_config.freq_hz < BNK16_HAPTIC_FREQ_MIN || kb_config.freq_hz > BNK16_HAPTIC_FREQ_MAX || kb_config.pulses < 1 || kb_config.pulses > BNK16_HAPTIC_PULSES_MAX || kb_config.knob_steps < 1 || kb_config.knob_steps > BNK16_BIG_STEPS_PER_KEY_MAX) {
        kb_config_set_defaults();
    }
}

static void haptic_save_and_buzz(void) {
    eeconfig_update_kb(kb_config.raw);
    haptic_buzz();
}

/* Called by the custom keycodes in keymap.c; every change gives a sample burst */
void bnk16_haptic_reset(void) {
    bool show_unassigned = kb_config.show_unassigned;
    kb_config_set_defaults();
    kb_config.show_unassigned = show_unassigned;
    haptic_save_and_buzz();
}

void bnk16_toggle_show_unassigned(void) {
    kb_config.show_unassigned = !kb_config.show_unassigned;
    eeconfig_update_kb(kb_config.raw);
}

bool bnk16_show_unassigned(void) {
    return kb_config.show_unassigned;
}

void bnk16_haptic_toggle_brake(void) {
    kb_config.brake = !kb_config.brake;
    haptic_save_and_buzz();
}

void bnk16_haptic_change_freq(bool up) {
    uint16_t freq = kb_config.freq_hz;
    if (up ? freq + BNK16_HAPTIC_FREQ_STEP <= BNK16_HAPTIC_FREQ_MAX : freq - BNK16_HAPTIC_FREQ_STEP >= BNK16_HAPTIC_FREQ_MIN) {
        kb_config.freq_hz = up ? freq + BNK16_HAPTIC_FREQ_STEP : freq - BNK16_HAPTIC_FREQ_STEP;
    }
    haptic_save_and_buzz();
}

void bnk16_haptic_change_length(bool up) {
    if (up ? kb_config.pulses < BNK16_HAPTIC_PULSES_MAX : kb_config.pulses > 1) {
        kb_config.pulses = up ? kb_config.pulses + 1 : kb_config.pulses - 1;
    }
    haptic_save_and_buzz();
}

/* Higher resolution = fewer steps per key press */
void bnk16_knob_change_resolution(bool up) {
    if (up ? kb_config.knob_steps > 1 : kb_config.knob_steps < BNK16_BIG_STEPS_PER_KEY_MAX) {
        kb_config.knob_steps = up ? kb_config.knob_steps - 1 : kb_config.knob_steps + 1;
    }
    haptic_save_and_buzz();
}

bool bnk16_haptic_brake_on(void) {
    return kb_config.brake;
}

uint8_t bnk16_knob_steps(void) {
    return kb_config.knob_steps;
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
    /* PB11 = BT/USB slide switch (the switch also cuts battery +), low = BT position, see mode_switch_task() */
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

/* Samples the knob at 4 kHz in an interrupt, see TMR2_IRQHandler */
static void knob_timer_start(void) {
    TMR2_TimerInit(FREQ_SYS / 4000);
    TMR2_ClearITFlag(RB_TMR_IF_CYC_END);
    TMR2_ITCfg(ENABLE, RB_TMR_IE_CYC_END);
    PFIC_EnableIRQ(TMR2_IRQn);
}

/* Before the shutdown: the wake-up pins take over */
static void knob_timer_stop(void) {
    PFIC_DisableIRQ(TMR2_IRQn);
    TMR2_ITCfg(DISABLE, RB_TMR_IE_CYC_END);
    R8_TMR2_CTRL_MOD = RB_TMR_ALL_CLEAR;
    TMR2_ClearITFlag(RB_TMR_IF_CYC_END);
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
        knob_timer_start();
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
    if (++step_count < kb_config.knob_steps) {
        return;
    }
    step_count = 0;
    if (timer_elapsed32(last_key_time) >= BNK16_BIG_MIN_KEY_INTERVAL_MS && !encoder_queue_full()) {
        last_key_time = timer_read32();
        encoder_queue_event(BNK16_BIG_ENCODER, !positive);
        haptic_buzz();
    }
}

/* Runs every scan from matrix_scan_kb(), before encoder_task() in the same keyboard_task() pass: hands the
 * steps counted by the interrupt to big_knob_step (a positive step is what QMK's own decoder reports as
 * ENCODER_COUNTER_CLOCKWISE, i.e. false). */
static void big_knob_scan(void) {
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
}
#endif

/* Every matrix scan, after the debounce and before the key events of that scan */
void matrix_scan_kb(void) {
    bnk16_sleep_diag_scan();
    layer_switch_task();
#ifdef ENCODER_ENABLE
    big_knob_scan();
#endif
    matrix_scan_user();
}

/* ========================================================================
 * Deep sleep and shutdown (power_management.c decides when). Both stop the knob timer, drive the rows
 * low and arm the key and knob pins as wake-up pins: any key then pulls its column low (COL2ROW diodes).
 * Only PB0-PB15 can wake the CH585: the small right knob (PB16/PB17) does not, its click does (row
 * PB21 x column PB4). A pull-up against a closed contact draws tens of microamps (phase 39b: 200 uA in
 * all), so PB10 (layer switch, low through a diode in positions 1-3) and PB11 (slide switch, at GND in
 * the BT position) float and do not wake.
 *
 * Deep sleep (BNK16_DEEP_SLEEP, mcuconf.h): the link stays up, the BLE stack sleeps the chip between
 * radio events (CH58X_LowPower in sdk/HAL/SLEEP.c, allowed by hal_sleep_allowed() in power_management.c)
 * and a pin interrupt wakes it. The pins are set as for the shutdown (test A: 70 uA asleep without BLE),
 * except that an encoder pin resting on a closed contact keeps its pull-up and wakes on the rising edge,
 * so every knob step wakes the board. The USB HS PHY and its PLL are switched off (test B, which
 * slept with the link kept, had no USB running; phase 38 left them on). PB11 is sampled every
 * DEEP_SLEEP_SWITCH_POLL_MS instead of being armed, see bnk16_deep_sleep_poll().
 *
 * Shutdown, the way the stock firmware sleeps: end the link, then switch the chip off (LowPower_Shutdown,
 * no RAM kept). A wake-up pin restarts it from reset, and the normal boot reconnects. An encoder pin on
 * a closed contact gets a pull-down and does not wake: the knob wakes once a contact that rests open
 * closes.
 * ======================================================================== */
#define SHUTDOWN_DISCONNECT_MS    300 /* lets the stack tell the host the link ends */
#define DEEP_SLEEP_SWITCH_POLL_MS 500 /* PB11 check in deep sleep */

static const pin_t sleep_row_pins[] = MATRIX_ROW_PINS;
static const pin_t sleep_col_pins[] = { B5, B4, B6, B7 };
static const pin_t sleep_enc_pins[] = { B14, B15, B8, B9 };

/* pin_t is the GPIO_Pin_x mask with bit 31 set for port B */
static inline uint32_t port_b_bit(pin_t pin) {
    return pin & 0x7FFFFFFF;
}

static volatile bool sleep_woken;

/* Deep sleep: a wake-up pin moved. The shutdown keeps interrupts off from the arming on, so there only
 * the pending flags do the waking. */
__INTERRUPT __HIGH_CODE void GPIOB_IRQHandler(void) {
    bnk16_sleep_diag_gpio_irq();
    R16_PB_INT_EN = 0; /* one wake-up is enough, the pins go back to QMK */
    R16_PB_INT_IF = 0xFFFF;
    sleep_woken   = true;
}

/* USB in use or a haptic burst still running: stay on */
bool bnk16_shutdown_possible(void) {
    return !usb_is_connected() && !haptic_running;
}

/* Rows low, columns armed on the falling edge, PB10/PB11 floating. Encoder pins on an open contact wake
 * on the falling edge; on a closed one they wake on the rising edge (deep sleep) or get a pull-down and
 * do not wake (shutdown). Returns true if a key is already down: it has no edge left to wake the chip. */
static bool sleep_pins_arm(bool shutdown) {
#ifdef ENCODER_ENABLE
    knob_timer_stop();
#endif
    PFIC_DisableIRQ(GPIO_B_IRQn);
    R16_PB_INT_EN = 0;

    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_row_pins); i++) {
        gpio_write_pin_low(sleep_row_pins[i]);
        gpio_set_pin_output(sleep_row_pins[i]);
    }
    gpio_set_pin_input(B10);
    gpio_set_pin_input(B11);
    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_col_pins); i++) {
        GPIOB_ITModeCfg(port_b_bit(sleep_col_pins[i]), GPIO_ITMode_FallEdge);
    }
    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_enc_pins); i++) {
        pin_t pin = sleep_enc_pins[i];
        if (gpio_read_pin(pin)) {
            GPIOB_ITModeCfg(port_b_bit(pin), GPIO_ITMode_FallEdge);
        } else if (!shutdown) {
            GPIOB_ITModeCfg(port_b_bit(pin), GPIO_ITMode_RiseEdge);
        } else {
            gpio_set_pin_input_low(pin);
        }
    }

    bool key_down = false;
    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_col_pins); i++) {
        if (!gpio_read_pin(sleep_col_pins[i])) {
            key_down = true;
        }
    }
    return key_down;
}

__attribute__((noreturn)) void bnk16_shutdown(void) {
    ble_radio_off();
    uint32_t start = timer_read32();
    while (timer_elapsed32(start) < SHUTDOWN_DISCONNECT_MS) {
        TMOS_SystemProcess();
        bnk16_sleep_diag_feed();
    }

    unsigned long irq_status;
    SYS_DisableAllIrq(&irq_status);
    bool     key_down  = sleep_pins_arm(true);
    uint16_t wake_mask = R16_PB_INT_EN;

    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_GPIO_WAKE;
    sys_safe_access_disable();
    PFIC_EnableIRQ(GPIO_B_IRQn);

    /* A key pressed by now has no edge left to wake the chip: reboot instead. Only the armed pins count:
     * the flags of the other port B pins latch too (phase 39: PB10 of the layer switch rebooted every
     * shutdown attempt). */
    if (key_down || (R16_PB_INT_IF & wake_mask)) {
        SYS_ResetExecute();
    }
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL = RB_WAKE_EV_MODE | RB_SLP_GPIO_WAKE;
    sys_safe_access_disable();
    bnk16_sleep_diag_shutdown();
    LowPower_Shutdown(0);
    SYS_ResetExecute(); /* not reached: LowPower_Shutdown() resets after the wake-up */
    __builtin_unreachable();
}

#ifdef BNK16_DEEP_SLEEP
static uint8_t  sleep_wake_ctrl; /* R8_SLP_WAKE_CTRL before the deep sleep */
static uint32_t sleep_switch_poll;

/* The USB HS port off (PHY suspended, its 480 MHz PLL and pins off). USBHS_Device_Init(DISABLE) cannot be
 * used: it also clears the system clock bits in R16_CLK_SYS_CFG. Only called with no host enumerated. */
static void usbhs_off(void) {
    PFIC_DisableIRQ(USB2_DEVICE_IRQn);
    R8_USB2_CTRL |= USBHS_UD_RST_SIE;
    R8_USB2_CTRL = 0; /* device, DMA and LPM off, PHY suspended (USBHS_UD_PHY_SUSPENDM low) */
    R8_USBHS_PLL_CTRL &= ~USBHS_PLL_EN;
    R16_PIN_CONFIG &= ~RB_PIN_USB2_EN;
}

/* USB HS on again after the deep sleep. Its block was off through the chip sleep and comes back with
 * flags its handler cannot clear (phase 47m: USB2 interrupt storm right after USBHS_Device_Init(), the
 * transfer-done flag is read-only and clears only with the endpoint state). So: PLL on first, reset the
 * protocol engine (endpoints included), then init, then drop every flag and the pending bit, all with
 * interrupts off globally (CSR 0x800; not SYS_DisableAllIrq(): its restore switches on what we switch off). */
static void usbhs_on_clean(void) {
    /* the firmware runs in user mode: mstatus is not accessible (47n: illegal instruction); WCH's
     * global interrupt enable CSR 0x800 is, as the SDK's flash code uses it */
    uint32_t gint;
    __asm__ volatile("csrrc %0, 0x800, %1" : "=r"(gint) : "r"(0x88));
    PFIC_DisableIRQ(USB2_DEVICE_IRQn);
    R8_USBHS_PLL_CTRL = USBHS_PLL_EN;
    DelayUs(100);
    R8_USB2_CTRL = USBHS_UD_RST_SIE | USBHS_UD_RST_LINK | USBHS_UD_CLR_ALL;
    DelayUs(10);
    R8_USB2_CTRL = 0;
    R8_USB2_INT_FG = 0xFF;
    USBHS_Device_Init(ENABLE); /* enables the interrupt at its end */
    R8_USB2_INT_FG = 0xFF;
    PFIC_ClearPendingIRQ(USB2_DEVICE_IRQn);
    __asm__ volatile("csrs 0x800, %0" ::"r"(gint & 0x88));
}

/* Board state for the chip sleep, see the section comment */
void bnk16_deep_sleep_enter(void) {
    unsigned long irq_status;
    BNK16_STAGE(STAGE_DS_ENTER);
    bnk16_sleep_diag_ds_enter();
    /* The USB HS and knob timer interrupts go off first, so that they are not in the state that
     * SYS_RecoverIrq() switches on again below. With the USB HS block off, the flag the chip sleep leaves
     * in it cannot be cleared, and its handler ran without end (phase 47l: IRQ 37 pending and enabled,
     * CPU stuck at the end of SYS_RecoverIrq() after the first wake-up). */
    PFIC_DisableIRQ(USB2_DEVICE_IRQn);
    PFIC_DisableIRQ(TMR2_IRQn);
    SYS_DisableAllIrq(&irq_status);
    bnk16_sleep_diag_ds_enter_irq_off();
    sleep_woken = false;
    usbhs_off();
    ws2812_power_toggle(false); /* already off in sleep; PA11 low = LED supply off */
    gpio_write_pin_low(WS2812_DI_PIN);
    bool key_down = sleep_pins_arm(false);

    sleep_wake_ctrl = R8_SLP_WAKE_CTRL;
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_WAKE_EV_MODE | RB_SLP_GPIO_WAKE; /* as PWR_PeriphWakeUpCfg() sets it */
    sys_safe_access_disable();
    R16_PB_INT_IF = 0xFFFF; /* drop flags latched before the arming, also those of unarmed pins */
    PFIC_EnableIRQ(GPIO_B_IRQn);
    if (key_down) {
        sleep_woken = true;
    }
    sleep_switch_poll = timer_read32();
    BNK16_STAGE(STAGE_DS_ARMED);
    bnk16_sleep_diag_ds_armed();
    SYS_RecoverIrq(irq_status);
}

/* Called in deep sleep once per pass of the main loop (about once per radio event). True once the board
 * has to wake up: a wake-up pin moved, or the slide switch is in the USB position. PB11 floats in deep
 * sleep (its pull-up against GND would cost about as much as the whole sleep), so its pull-up is turned
 * on for a few microseconds now and then. */
bool bnk16_deep_sleep_poll(void) {
    BNK16_STAGE(STAGE_DS_LOOP);
    if (bnk16_sleep_diag_ds_poll()) { /* test builds: timed exit */
        sleep_woken = true;
    }
    /* the BLE stack may enable the RF antenna switch outputs (PB16-PB21) later on; PB21 is a matrix row */
    if (R16_PIN_ALTERNATE & RB_RF_ANT_SW_EN) {
        R16_PIN_ALTERNATE &= ~RB_RF_ANT_SW_EN;
    }
    if (!sleep_woken && timer_elapsed32(sleep_switch_poll) >= DEEP_SLEEP_SWITCH_POLL_MS) {
        sleep_switch_poll = timer_read32();
        gpio_set_pin_input_high(B11);
        DelayUs(20);
        if (gpio_read_pin(B11)) {
            sleep_woken = true;
        }
        gpio_set_pin_input(B11);
    }
    return sleep_woken;
}

/* Read by hal_sleep_allowed() with interrupts off */
__HIGH_CODE bool bnk16_deep_sleep_woken(void) {
    return sleep_woken;
}

/* Back to the pin states QMK expects between scans (rows unselected), knob timer and USB on again */
void bnk16_deep_sleep_exit(void) {
    BNK16_STAGE(STAGE_DS_EXIT);
    bnk16_sleep_diag_ds_exit();
    PFIC_DisableIRQ(GPIO_B_IRQn);
    R16_PB_INT_EN = 0;
    R16_PB_INT_IF = 0xFFFF;
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL = sleep_wake_ctrl;
    sys_safe_access_disable();

    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_row_pins); i++) {
        gpio_set_pin_input_high(sleep_row_pins[i]);
    }
    for (uint8_t i = 0; i < ARRAY_SIZE(sleep_enc_pins); i++) {
        gpio_set_pin_input_high(sleep_enc_pins[i]);
    }
    gpio_set_pin_input_high(B10);
    gpio_set_pin_input_high(B11);
#ifdef ENCODER_ENABLE
    knob_timer_start();
#endif
    /* also needed after any sleep with the USB/BLE power domain kept (RB_PWR_EXTEND), says the SDK */
    BNK16_STAGE(STAGE_DS_EXIT_USB);
    usbhs_on_clean();
    BNK16_STAGE(STAGE_DS_EXIT_DONE);
    bnk16_sleep_diag_ds_exit_done();
}
#endif

int main(void) {
    extern void protocol_setup();
    extern void protocol_pre_init();
    extern void protocol_post_init();
    extern void platform_run();

    bnk16_sleep_diag_boot(); /* test builds only; reads the reset cause before anything changes it */
    platform_setup();

    bnk16_pins_prepare();

    /* LED supply off until the RGB driver switches it on with the first frame */
    gpio_write_pin(WS2812_EN_PIN, !WS2812_EN_LEVEL);
    gpio_set_pin_output(WS2812_EN_PIN);

    protocol_setup();
#if !defined ESB_ENABLE || ESB_ENABLE != 2
    keyboard_setup();
#endif

    protocol_pre_init();
    keyboard_init();
    kb_config_load();
    protocol_post_init();

    battery_init();
    power_init();

    /* the mode comes from the switch; TMOS runs from the loop below, so BLE has not started anything yet */
    wired_mode_active = mode_switch_is_usb();
    power_set_wired_mode(wired_mode_active);
    ble_restore_last_mode();

    for (;;) {
        platform_run();
        //! housekeeping_task() is handled by platform

        /* sleep until the next interrupt; peripherals and RAM keep running. Not in deep sleep: there the
         * chip sleeps inside TMOS, and with the knob timer stopped and the RTC interrupt consumed inside
         * CH58X_LowPower() this WFI may have nothing left to end it (WCH's examples never use one) */
#ifdef BNK16_DEEP_SLEEP
        if (power_in_deep_sleep()) {
            continue;
        }
#endif
        __WFI();
    }
}
