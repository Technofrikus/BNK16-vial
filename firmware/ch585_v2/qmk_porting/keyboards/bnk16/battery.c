/*
 * battery.c — battery voltage and level
 *
 * PA6 (AIN10) is the battery through a 1:2 divider, always on (phase 40c). Calibrated on the board
 * (2026-11-03) with a bench supply on the battery connector, 3.0-4.2 V in 0.1 V steps: the SDK's
 * -12 dB conversion of PA6 times BATTERY_CAL_GAIN plus BATTERY_CAL_OFFSET_MV matched the multimeter
 * within 10 mV. The ADC reads PA6 against its internal reference, so the result holds below 3.35 V too,
 * where the LDO drops out and the chip supply falls with the battery.
 *
 * There is no charger status or VBUS pin: while charging PA6 sits at the charger's 4.2 V, so the level
 * shows 100 % and only means something on battery.
 */

#include "battery.h"
#include "quantum.h"
#include "CH58x_common.h"

#define BATTERY_AIN                 10    /* PA6 */
#define BATTERY_CAL_GAIN_X10000     19844 /* battery mV per PA6 mV, x 10000 */
#define BATTERY_CAL_OFFSET_MV       25
#define BATTERY_MEASURE_INTERVAL_MS 10000
#define BATTERY_SAMPLES             16

/* Battery mV to percent, a common LiPo curve at light load; linear in between */
static const struct {
    uint16_t mv;
    uint8_t  percent;
} battery_curve[] = {
    { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 }, { 3980, 75 }, { 3950, 70 },
    { 3910, 65 },  { 3870, 60 }, { 3850, 55 }, { 3840, 50 }, { 3820, 45 }, { 3800, 40 }, { 3790, 35 },
    { 3770, 30 },  { 3750, 25 }, { 3730, 20 }, { 3710, 15 }, { 3690, 10 }, { 3610, 5 },
    { BATTERY_EMPTY_VOLTAGE_MV, 0 },
};

static uint16_t battery_mv;    /* smoothed, 0 = not measured yet */
static uint8_t  battery_level = 100;
static uint32_t battery_last_measure;
static uint8_t  battery_measurements; /* up to BATTERY_CRITICAL_MIN_MEASUREMENTS */

/* the smoothing needs a few readings before a low value is trusted for a shutdown */
#define BATTERY_CRITICAL_MIN_MEASUREMENTS 4

static uint16_t battery_read_mv(void)
{
    uint32_t sum = 0;

    ADC_ExtSingleChSampInit(SampleFreq_4_or_2, ADC_PGA_1_4);
    ADC_ChannelCfg(BATTERY_AIN);
    ADC_ExcutSingleConver(); /* the first conversion after a channel change is dropped */
    for (uint8_t i = 0; i < BATTERY_SAMPLES; i++) {
        sum += ADC_ExcutSingleConver();
    }
    R8_ADC_CFG &= ~(RB_ADC_BUF_EN | RB_ADC_POWER_ON);

    int pin_mv = ADC_VoltConverSignalPGA_MINUS_12dB(sum / BATTERY_SAMPLES);
    return pin_mv * BATTERY_CAL_GAIN_X10000 / 10000 + BATTERY_CAL_OFFSET_MV;
}

static uint8_t battery_mv_to_percent(uint16_t mv)
{
    if (mv >= battery_curve[0].mv) {
        return 100;
    }
    for (uint8_t i = 1; i < ARRAY_SIZE(battery_curve); i++) {
        if (mv >= battery_curve[i].mv) {
            uint16_t hi_mv = battery_curve[i - 1].mv, lo_mv = battery_curve[i].mv;
            uint8_t  hi_p = battery_curve[i - 1].percent, lo_p = battery_curve[i].percent;
            return lo_p + (uint32_t)(mv - lo_mv) * (hi_p - lo_p) / (hi_mv - lo_mv);
        }
    }
    return 0;
}

/* Measures every BATTERY_MEASURE_INTERVAL_MS, cheap to call more often. The voltage is smoothed (LED load
 * and radio bursts), and the level moves only by 2 % or more, or to 0/100, so it does not flicker. */
static void battery_update(void)
{
    if (battery_mv && timer_elapsed32(battery_last_measure) < BATTERY_MEASURE_INTERVAL_MS) {
        return;
    }
    battery_last_measure = timer_read32();

    uint16_t mv = battery_read_mv();
    battery_mv  = battery_mv ? (battery_mv * 3 + mv) / 4 : mv;
    if (battery_measurements < BATTERY_CRITICAL_MIN_MEASUREMENTS) {
        battery_measurements++;
    }

    uint8_t percent = battery_mv_to_percent(battery_mv);
    int     diff    = (int)percent - battery_level;
    if (diff >= 2 || diff <= -2 || percent == 0 || percent == 100) {
        battery_level = percent;
    }
}

void battery_init(void)
{
    gpio_set_pin_input(A6); /* no pull, it would load the divider */
    battery_update();
}

uint8_t battery_get_level(void)
{
    battery_update();
    return battery_level;
}

uint16_t battery_get_voltage_mv(void)
{
    battery_update();
    return battery_mv;
}

bool battery_is_critical(void)
{
    battery_update();
    return battery_measurements >= BATTERY_CRITICAL_MIN_MEASUREMENTS && battery_mv < BATTERY_SHUTDOWN_VOLTAGE_MV;
}

bool battery_is_charging(void)
{
    return false; /* no charger status pin */
}

bool battery_is_battery_powered(void)
{
    return false; /* no VBUS pin */
}
