/*
 * BNK16 bring-up diagnostics, compiled only with BNK16_PIN_SCAN or BNK16_SWITCH_PROBE (see qmk_config.h).
 * They find pins that are still unknown by typing their results over USB, so open a text editor first.
 * Never enable them in a normal build: they drive free pins and type on their own.
 */
#include "bnk16_diag.h"

#if defined(BNK16_PIN_SCAN) || defined(BNK16_SWITCH_PROBE) || defined(BNK16_INTERVAL_DEBUG) || defined(BNK16_SLEEP_DIAG) || defined(BNK16_VBUS_PROBE) || defined(BNK16_CABLE_PROBE)
#include QMK_KEYBOARD_H
#include "CH58x_common.h"
#include "ble_app.h"
#include "power_management.h"
#include <string.h>
#include <stdio.h>

#ifdef BNK16_PIN_SCAN
/* ========================================================================
 * Pin scan: find what an unknown output pin switches (found the LED supply PA11).
 * Starts automatically 5 s after boot. Every candidate pin is driven high for 1 s, then
 * buzzed with a 170-250 Hz square wave for 1 s (the stock haptic PWM range; a coin LRA only
 * vibrates on AC, and the square wave also turns on a low-side driven P-FET). Its name is typed
 * over USB in lower case without Shift or Enter ("a3 h ", "a3 p ").
 * Excluded: matrix, encoders, layer/mode switch, PA7 (haptics), PA10 (LED data), USB, PB22.
 * ======================================================================== */
static const struct { pin_t pin; const char *name; } scan_pins[] = {
    { A0, "a0" },
    { A1, "a1" },
    { A2, "a2" },
    { A3, "a3" },
    { A4, "a4" },
    { A5, "a5" },
    { A6, "a6" },
    { A8, "a8" },
    { A9, "a9" },
    { A11, "a11" },
    { A12, "a12" },
    { A13, "a13" },
    { A14, "a14" },
    { A15, "a15" },
    { B12, "b12" },
    { B13, "b13" },
    { B18, "b18" },
    { B19, "b19" },
    { B20, "b20" },
    { B23, "b23" }
};
#define SCAN_N (sizeof(scan_pins) / sizeof(scan_pins[0]))
#define SCAN_STEP_MS 1000

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
    gpio_write_pin(scan_pins[scan_idx].pin, 1);
    scan_type(scan_pins[scan_idx].name);
    scan_type(scan_phase == 0 ? " h " : " p ");
    scan_t = timer_read32();
}

/* One 250 ms burst per call, stepping through the frequencies, so the main loop keeps running. */
static void scan_buzz(void) {
    static const uint16_t half_us[] = { 2000, 2130, 2500, 2940 }; /* 250, 235, 200, 170 Hz */
    static uint8_t        freq      = 0;
    uint16_t              half      = half_us[freq];

    freq = (freq + 1) % (sizeof(half_us) / sizeof(half_us[0]));
    for (uint32_t t = 0; t < 250000; t += half) {
        gpio_toggle_pin(scan_pins[scan_idx].pin);
        wait_us(half);
    }
}

static void pin_scan_task(void) {
    static bool started = false;

    /* Starts by itself 5 s after boot (time to focus a text field); no key is involved,
     * so no physical key can be held while the report is rewritten. */
    if (!started && timer_read32() > 5000) {
        started    = true;
        scan_idx   = 0;
        scan_phase = 0;
        scan_type("start ");
        scan_begin_phase();
    }

    if (scan_idx < 0) {
        return;
    }
    if (timer_elapsed32(scan_t) < SCAN_STEP_MS) {
        if (scan_phase == 1) {
            scan_buzz();
        }
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
 * Excluded: PA10/PA11 (LEDs), PB10/PB11 (layer and mode switch), PB12/PB13 (USB HS), PB23 (reset).
 * ======================================================================== */
static const struct { pin_t pin; const char *name; } sp_pins[] = {
    /* free pins: pull-up, pull-down, driven */
    { A0, "a0" }, { A1, "a1" }, { A2, "a2" }, { A3, "a3" }, { A4, "a4" }, { A5, "a5" }, { A6, "a6" },
    { A8, "a8" }, { A9, "a9" }, { A12, "a12" }, { A13, "a13" }, { A14, "a14" }, { A15, "a15" },
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
#define SP_ADC     13 /* the first 13 free pins (PA0-PA6, PA8, PA9, PA12-PA15) are ADC inputs */
/* until phase 40d this table had AIN11 (= PA7) for a8, so every ADC label from a8 on was one pin off */
static const uint8_t sp_adc_channel[SP_ADC] = { 9, 8, 7, 6, 0, 1, 10, 12, 13, 2, 3, 4, 5 };
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

#ifdef BNK16_INTERVAL_DEBUG
/* ========================================================================
 * Interval report: shows whether the host accepted the slower intervals. Typed over BLE:
 * "start" 3 s after the first connection, which proves that typing from here works, then one line
 * 10 s after a key press with the state at the moment of the press; typing earlier, while the link is still
 * on a slow interval, loses characters. Only the first press of those 10 s is reported:
 * s1 asleep or s0 awake, n requests sent, r status of the last request (0 = accepted by the stack),
 * m its max interval, u updates seen on the link, i interval, l latency, t timeout.
 * Interval units are 1.25 ms, so 12 = 15 ms, 96 = 120 ms, 816 = 1020 ms.
 * ======================================================================== */
static ble_conn_debug_t key_snapshot;
static bool             key_asleep;
static bool             key_pending;
static uint32_t         key_time;

/* Called on every key press, before it can wake the board */
void bnk16_diag_note_key(void) {
    if (!key_pending) {
        key_snapshot = ble_conn_debug;
        key_asleep   = power_get_state() == POWER_STATE_SLEEPING;
        key_pending  = true;
        key_time     = timer_read32();
    }
}

static void interval_report_task(void) {
    static bool     started;
    static uint32_t connected_time;
    char            line[64];

    if (!ble_is_connected()) {
        connected_time = timer_read32();
        return;
    }
    if (!started && timer_elapsed32(connected_time) >= 3000) {
        started = true;
        send_string(" start\n");
        clear_keyboard();
    }
    if (key_pending && timer_elapsed32(key_time) >= 10000) {
        const ble_conn_debug_t *c = &key_snapshot;
        snprintf(line, sizeof(line), " s%u n%u r%u m%u u%u i%u l%u t%u\n", key_asleep, c->requests, c->request_status, c->requested_max, c->updates, c->interval, c->latency, c->timeout);
        send_string(line);
        clear_keyboard();
        key_pending = false;
    }
}
#endif

#ifdef BNK16_SLEEP_DIAG
/* ========================================================================
 * Sleep report: one line typed over BLE 5 s after every connection and 2 s after every wake-up from
 * deep sleep, counters since boot:
 * k breadcrumb found at boot (DIAG_CRUMB_x in bnk16_diag.h; 00 = power was really lost),
 * b reset cause of this boot (R8_RESET_STATUS: 1 power on, 0 software, 2 watchdog, 3 reset pin,
 *   5 wake-up from a sleep that lost the RAM), e/x deep sleep entries/exits,
 * CH58X_LowPower results: v vetoed, s time out of range, r slept and woken by the RTC,
 *   p slept and woken by a pin, l wake-up time already passed,
 * t links ended, d reason of the last one (hex HCI code: 08 supervision timeout, 13 host ended it,
 *   22/28 link layer timeout / instant passed, 3e failed to establish).
 * If the board is in deep sleep when it reconnects, the line comes after the next key.
 * ======================================================================== */
#include "SLEEP.h"

static uint8_t crumb_at_boot;

/* Runs first thing in main(): keep the breadcrumb the last run left behind */
void bnk16_diag_boot(void) {
    crumb_at_boot = R8_GLOB_RESET_KEEP;
    DIAG_CRUMB(DIAG_CRUMB_BOOT);
}

static void sleep_report_task(void) {
    static uint32_t connected_time;
    static bool     reported;
    static uint16_t last_exits;
    static uint32_t exit_time;
    char            line[112];

    if (power_deep_sleep_exits != last_exits) {
        last_exits = power_deep_sleep_exits;
        exit_time  = timer_read32();
        reported   = false;
    }
    if (!ble_is_connected()) {
        connected_time = timer_read32();
        reported       = false;
        return;
    }
    if (reported || timer_elapsed32(connected_time) < 5000 || (last_exits != 0 && timer_elapsed32(exit_time) < 2000)) {
        return;
    }
    reported = true;
    snprintf(line, sizeof(line), " k%02x b%u e%u x%u v%lu s%lu r%lu p%lu l%lu t%u d%02x\n", crumb_at_boot, R8_RESET_STATUS & RB_RESET_FLAG, power_deep_sleep_entries, power_deep_sleep_exits, hal_sleep_stats[HAL_SLEEP_STAT_VETO], hal_sleep_stats[HAL_SLEEP_STAT_SHORT], hal_sleep_stats[HAL_SLEEP_STAT_RTC], hal_sleep_stats[HAL_SLEEP_STAT_PIN], hal_sleep_stats[HAL_SLEEP_STAT_LATE], ble_conn_debug.terminations, ble_conn_debug.term_reason);
    send_string(line);
    clear_keyboard();
}
#endif

#ifdef BNK16_VBUS_PROBE
/* ========================================================================
 * Cable sense probe (the switch probe showed PA6 tied high on battery, and all pull-ups higher with a cable).
 * Every 50 ms: PA6 level without pull, PA6 voltage and the chip supply (internal VBAT channel), in mV.
 * Every 3 s one line with the range of those 3 s: " a6 <level flips> <min>-<max> vdd <min>-<max>".
 * Phase 40b showed PA6 at about half the supply, so it is a divider, and with a cable the values never
 * settled. Starts 5 s after boot.
 * ======================================================================== */
#define VBUS_PROBE_AIN_A6 10

static int vbus_probe_mv(void) {
    uint32_t sum = 0;
    ADC_ExcutSingleConver();
    for (uint8_t n = 0; n < 8; n++) {
        sum += ADC_ExcutSingleConver();
    }
    return ADC_VoltConverSignalPGA_MINUS_12dB(sum / 8);
}

static void vbus_probe_task(void) {
    static uint32_t last_sample, window_start;
    static int      last_level = -1, flips, a6_min = 99999, a6_max, vdd_min = 99999, vdd_max;
    char            line[64];

    if (timer_read32() < 5000 || timer_elapsed32(last_sample) < 50) {
        return;
    }
    last_sample = timer_read32();

    gpio_set_pin_input(A6);
    int level = gpio_read_pin(A6) ? 1 : 0;
    ADC_ExtSingleChSampInit(SampleFreq_4_or_2, ADC_PGA_1_4);
    ADC_ChannelCfg(VBUS_PROBE_AIN_A6);
    int a6 = vbus_probe_mv();
    ADC_InterBATSampInit();
    int vdd = vbus_probe_mv();

    if (last_level >= 0 && level != last_level) {
        flips++;
    }
    last_level = level;
    a6_min     = MIN(a6_min, a6);
    a6_max     = MAX(a6_max, a6);
    vdd_min    = MIN(vdd_min, vdd);
    vdd_max    = MAX(vdd_max, vdd);

    if (timer_elapsed32(window_start) >= 3000) {
        snprintf(line, sizeof(line), " a6 %d %d-%d vdd %d-%d\n", flips, a6_min, a6_max, vdd_min, vdd_max);
        send_string_with_delay(line, 20);
        clear_keyboard();
        window_start = timer_read32();
        flips        = 0;
        a6_min = vdd_min = 99999;
        a6_max = vdd_max = 0;
    }
}
#endif

#ifdef BNK16_CABLE_PROBE
/* ========================================================================
 * Cable probe (phase 40d/40e): looks for a VBUS sense pin and the charger status pin. On the PCB layout the stock
 * firmware was built for, VBUS is a pin without pull (high = cable) and the charger status an open drain that
 * is low while charging, read only with VBUS present. Phase 40 lost the start of every line with the cable in,
 * which is where its pin levels were, so the cable case was never really seen.
 * One candidate per 25 ms (each pin every 400 ms; 40d used 6 ms and the link kept dropping) is read four times:
 * with pull-down, floating after it, with pull-up, floating after it. So "0000" = driven low, "1111" = driven
 * high, "0011" = not driven (keeps the last pull), a VBUS divider is low without cable and high with it,
 * a charger status pin reads "0000" while charging and "0011" otherwise. ADC pins are also measured floating,
 * in mV (-12 dB conversion, so up to about 4.2 V). A code counts once it was the same 5 times in a row (2 s).
 * Lines are typed over BLE (BT position) once nothing changed for 5 s, so plugging the cable cannot eat them,
 * after 3 spaces as sacrificial characters, with the battery voltage (4.2 V = charging):
 *   n0 bat 4012 a0 0011 v1650 ... b22 1111     every pin, once 3 s after connecting
 *   n3 bat 4195 a3 0000 v0                     later only the pins that changed
 * A pin that changed more than 6 times within 20 s is reported once as "a3x" and then left out (floating noise);
 * in 40d such a pin kept the 5 s quiet timer from ever running out, so no line came after the first.
 * Keeps the board awake (no sleep, no shutdown) while it runs.
 * ======================================================================== */
#include "battery.h"

static const struct { pin_t pin; const char *name; int8_t ain; } cp_pins[] = {
    { A0, "a0", 9 }, { A1, "a1", 8 }, { A2, "a2", 7 }, { A3, "a3", 6 }, { A4, "a4", 0 }, { A5, "a5", 1 },
    { A8, "a8", 12 }, { A9, "a9", 13 }, { A12, "a12", 2 }, { A13, "a13", 3 }, { A14, "a14", 4 }, { A15, "a15", 5 },
    { B18, "b18", -1 }, { B19, "b19", -1 }, { B20, "b20", -1 }, { B22, "b22", -1 },
};
#define CP_N              (sizeof(cp_pins) / sizeof(cp_pins[0]))
#define CP_STEP_MS        25
#define CP_SETTLE_US      100
#define CP_STABLE_SAMPLES 5
#define CP_QUIET_MS       5000
#define CP_CONNECTED_MS   3000
#define CP_MV_STEP        300
#define CP_MAX_CHANGES    6
#define CP_NOISE_WINDOW_MS 20000
#define CP_UNKNOWN        0xFF

static struct {
    uint8_t code, candidate, count, changes;
    bool    dirty, noisy, noisy_shown;
    int16_t mv, mv_shown; /* smoothed, -1 = not measured yet */
} cp[CP_N];

static uint8_t cp_read_code(pin_t pin) {
    uint8_t code = 0;
    gpio_set_pin_input_low(pin);
    wait_us(CP_SETTLE_US);
    code |= gpio_read_pin(pin) << 3;
    gpio_set_pin_input(pin);
    wait_us(CP_SETTLE_US);
    code |= gpio_read_pin(pin) << 2;
    gpio_set_pin_input_high(pin);
    wait_us(CP_SETTLE_US);
    code |= gpio_read_pin(pin) << 1;
    gpio_set_pin_input(pin); /* left floating, so a divider is not loaded */
    wait_us(CP_SETTLE_US);
    code |= gpio_read_pin(pin);
    return code;
}

static int16_t cp_read_mv(uint8_t ain) {
    uint32_t sum = 0;
    ADC_ExtSingleChSampInit(SampleFreq_4_or_2, ADC_PGA_1_4);
    ADC_ChannelCfg(ain);
    ADC_ExcutSingleConver();
    for (uint8_t n = 0; n < 8; n++) {
        sum += ADC_ExcutSingleConver();
    }
    R8_ADC_CFG &= ~(RB_ADC_BUF_EN | RB_ADC_POWER_ON);
    return ADC_VoltConverSignalPGA_MINUS_12dB(sum / 8);
}

static void cable_probe_task(void) {
    static bool     started, full_done;
    static uint8_t  next;
    static uint16_t count;
    static uint32_t last_step, last_change, connected_time, noise_window;
    static char     line[320];

    if (!started) {
        if (timer_read32() < 5000) {
            return;
        }
        started = true;
        GPIOADigitalCfg(ENABLE, GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15);
        GPIOBDigitalCfg(ENABLE, GPIO_Pin_18 | GPIO_Pin_19 | GPIO_Pin_20 | GPIO_Pin_22);
        for (uint8_t i = 0; i < CP_N; i++) {
            cp[i].code = cp[i].candidate = CP_UNKNOWN;
            cp[i].mv = cp[i].mv_shown = -1;
        }
        last_change = timer_read32();
    }
    power_notify_activity();

    if (timer_elapsed32(last_step) >= CP_STEP_MS) {
        last_step = timer_read32();
        uint8_t i = next;
        next      = (next + 1) % CP_N;

        if (i == 0 && timer_elapsed32(noise_window) >= CP_NOISE_WINDOW_MS) {
            noise_window = timer_read32();
            for (uint8_t j = 0; j < CP_N; j++) {
                cp[j].changes = 0;
            }
        }

        bool    changed = false;
        uint8_t code    = cp_read_code(cp_pins[i].pin);
        if (code != cp[i].candidate) {
            cp[i].candidate = code;
            cp[i].count     = 1;
        } else if (cp[i].count < CP_STABLE_SAMPLES && ++cp[i].count == CP_STABLE_SAMPLES && code != cp[i].code) {
            if (cp[i].code != CP_UNKNOWN) {
                cp[i].changes++;
            }
            cp[i].code  = code;
            cp[i].dirty = true;
            changed     = true;
        }

        if (cp_pins[i].ain >= 0) {
            int16_t mv = cp_read_mv(cp_pins[i].ain);
            cp[i].mv   = cp[i].mv < 0 ? mv : (cp[i].mv * 3 + mv) / 4;
            if (cp[i].mv_shown < 0 || cp[i].mv - cp[i].mv_shown >= CP_MV_STEP || cp[i].mv_shown - cp[i].mv >= CP_MV_STEP) {
                if (cp[i].mv_shown >= 0) {
                    cp[i].changes++;
                }
                cp[i].mv_shown = cp[i].mv;
                cp[i].dirty    = true;
                changed        = true;
            }
        }
        if (cp[i].changes > CP_MAX_CHANGES) {
            cp[i].noisy = true;
        }
        if (changed && !cp[i].noisy) {
            last_change = timer_read32();
        }
    }

    if (!ble_is_connected()) {
        connected_time = timer_read32();
        return;
    }
    if (timer_elapsed32(connected_time) < CP_CONNECTED_MS || timer_elapsed32(last_change) < CP_QUIET_MS) {
        return;
    }
    for (uint8_t i = 0; i < CP_N; i++) {
        if (cp[i].code == CP_UNKNOWN) {
            return;
        }
    }

    size_t len   = snprintf(line, sizeof(line), "   n%u bat %u", count, battery_get_voltage_mv());
    bool   found = !full_done;
    for (uint8_t i = 0; i < CP_N && len < sizeof(line); i++) {
        if (cp[i].noisy) {
            if (!cp[i].noisy_shown) {
                cp[i].noisy_shown = true;
                found             = true;
                len += snprintf(line + len, sizeof(line) - len, " %sx", cp_pins[i].name);
            }
            continue;
        }
        if (!full_done || cp[i].dirty) {
            found = true;
            len += snprintf(line + len, sizeof(line) - len, " %s %u%u%u%u", cp_pins[i].name, (cp[i].code >> 3) & 1, (cp[i].code >> 2) & 1, (cp[i].code >> 1) & 1, cp[i].code & 1);
            if (cp_pins[i].ain >= 0 && len < sizeof(line)) {
                len += snprintf(line + len, sizeof(line) - len, " v%d", cp[i].mv_shown);
            }
        }
        cp[i].dirty = false;
    }
    if (!found) {
        return;
    }
    full_done = true;
    count++;
    if (len < sizeof(line) - 1) {
        line[len++] = '\n';
        line[len]   = '\0';
    }
    send_string_with_delay(line, 20);
    clear_keyboard();
}
#endif

void bnk16_diag_task(void) {
#ifdef BNK16_CABLE_PROBE
    cable_probe_task();
#endif
#ifdef BNK16_VBUS_PROBE
    vbus_probe_task();
#endif
#ifdef BNK16_SLEEP_DIAG
    sleep_report_task();
#endif
#ifdef BNK16_INTERVAL_DEBUG
    interval_report_task();
#endif
#ifdef BNK16_PIN_SCAN
    pin_scan_task();
#endif
#ifdef BNK16_SWITCH_PROBE
    switch_probe_task();
#endif
}
#endif
