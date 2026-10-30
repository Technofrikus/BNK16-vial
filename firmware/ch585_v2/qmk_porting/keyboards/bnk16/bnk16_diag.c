/*
 * BNK16 bring-up diagnostics, compiled only with BNK16_PIN_SCAN or BNK16_SWITCH_PROBE (see qmk_config.h).
 * They find pins that are still unknown by typing their results over USB, so open a text editor first.
 * Never enable them in a normal build: they drive free pins and type on their own.
 */
#include "bnk16_diag.h"

#if defined(BNK16_PIN_SCAN) || defined(BNK16_SWITCH_PROBE)
#include QMK_KEYBOARD_H
#include "CH58x_common.h"
#include <string.h>
#include <stdio.h>

#ifdef BNK16_PIN_SCAN
/* ========================================================================
 * Pin scan: find what an unknown output pin switches (found the LED supply PA11).
 * Starts automatically 20 s after boot. Every candidate pin is driven
 * high for 3 s, then low for 5 s, and its name is typed over USB in lower case without Shift or Enter ("a3h ", "b12l ").
 * Excluded: matrix, encoders, PA10/PA11 (LED data/supply), USB, PB22.
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
    { A12, "a12" },
    { A13, "a13" },
    { A14, "a14" },
    { A15, "a15" },
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

void bnk16_diag_task(void) {
#ifdef BNK16_PIN_SCAN
    pin_scan_task();
#endif
#ifdef BNK16_SWITCH_PROBE
    switch_probe_task();
#endif
}
#endif
