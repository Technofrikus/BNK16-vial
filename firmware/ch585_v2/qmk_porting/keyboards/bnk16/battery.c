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
 * shows 100 % and only means something on battery, and that 4.2 V is the only sign of a cable.
 *
 * Two voltages are kept. battery_mv is what the cell really has at its terminals, smoothed: it decides
 * the shutdown and the charging state. battery_est_x16 is the same without the LED load and decides the
 * level: the LEDs pull the cell down by some 10 mV to over 100 mV, which is 5 % and more on the flat
 * middle of the curve, so the level would fall whenever they light up and climb back in sleep.
 */

#include "battery.h"
#include "quantum.h"
#include "CH58x_common.h"
#include "ws2812_supplement.h"

#define BATTERY_AIN                 10    /* PA6 */
#define BATTERY_CAL_GAIN_X10000     19844 /* battery mV per PA6 mV, x 10000 */
#define BATTERY_CAL_OFFSET_MV       25
#define BATTERY_MEASURE_INTERVAL_MS 10000
#define BATTERY_SAMPLES             16
/* After the LEDs came on the cell keeps sinking for a while, and after they went off it recovers. In that
 * time a change in the reading is the load's doing, not the charge's: the estimate is held. With the LEDs
 * on, the gap to the reading is learned as the sag of this load; a minute of real discharge is nothing. */
#define BATTERY_LOAD_SETTLE_MS      60000
#define BATTERY_REST_SETTLE_MS      20000
#define BATTERY_SAG_MAX_MV          200   /* more than the LEDs can cause: something else changed, e.g. the cable */
/* The level follows a fall of this many percent and a rise of that many: on battery it should only fall,
 * a rise is the cell recovering (small) or the charger (large) */
#define BATTERY_LEVEL_FALL_STEP     2
#define BATTERY_LEVEL_RISE_STEP     4

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
static uint32_t battery_est_x16;      /* smoothed voltage without the LED load, in 1/16 mV */
static uint16_t battery_sag_mv;       /* how far the LEDs pull the reading down, learned as they come on */
static bool     battery_loaded;       /* LED supply on */
static uint32_t battery_load_change;  /* when battery_loaded last changed */
static uint8_t  battery_level = 100;
static uint32_t battery_last_measure;
static uint8_t  battery_measurements; /* up to BATTERY_CRITICAL_MIN_MEASUREMENTS */
static uint16_t battery_peak_mv;      /* highest smoothed voltage since it rose to the charging level */

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

/* The voltage the cell would have without the LEDs, from a reading with or without them */
static void battery_estimate(uint16_t mv)
{
    if (!battery_est_x16) {
        battery_est_x16 = (uint32_t)mv << 4;
        return;
    }
    if (timer_elapsed32(battery_load_change) < (battery_loaded ? BATTERY_LOAD_SETTLE_MS : BATTERY_REST_SETTLE_MS)) {
        if (battery_loaded) {
            int sag        = (int)(battery_est_x16 >> 4) - mv;
            battery_sag_mv = sag < 0 ? 0 : sag > BATTERY_SAG_MAX_MV ? BATTERY_SAG_MAX_MV : sag;
        }
        return;
    }
    int32_t target = (int32_t)(mv + (battery_loaded ? battery_sag_mv : 0)) << 4;
    battery_est_x16 += (target - (int32_t)battery_est_x16) / 4;
}

/* Measures every BATTERY_MEASURE_INTERVAL_MS, cheap to call more often (the main loop does, through
 * power_task(), so a change of the LED supply is seen at once). Both voltages are smoothed (radio bursts,
 * haptics); the level comes from the one without the LED load and moves in steps, so it does not flicker. */
static void battery_update(void)
{
    bool loaded = ws2812_power_get();
    if (loaded != battery_loaded) {
        battery_loaded      = loaded;
        battery_load_change = timer_read32();
    }

    if (battery_mv && timer_elapsed32(battery_last_measure) < BATTERY_MEASURE_INTERVAL_MS) {
        return;
    }
    battery_last_measure = timer_read32();

    uint16_t mv = battery_read_mv();
    battery_mv  = battery_mv ? (battery_mv * 3 + mv) / 4 : mv;
    if (battery_mv < BATTERY_CHARGING_VOLTAGE_MV) {
        battery_peak_mv = 0;
    } else if (battery_mv > battery_peak_mv) {
        battery_peak_mv = battery_mv;
    }
    if (battery_measurements < BATTERY_CRITICAL_MIN_MEASUREMENTS) {
        battery_measurements++;
    }

    bool first = !battery_est_x16;
    battery_estimate(mv);

    uint8_t percent = battery_mv_to_percent((battery_est_x16 + 8) >> 4);
    int     diff    = (int)percent - battery_level;
    if (first || diff <= -BATTERY_LEVEL_FALL_STEP || diff >= BATTERY_LEVEL_RISE_STEP || (diff && (percent == 0 || percent == 100))) {
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

/* No charger status pin, see battery.h. The charger holds the cell flat at 4.2 V; a full cell without the
 * cable (or after the charger stopped) slowly falls from there, so a drop from the peak ends charging. */
bool battery_is_charging(void)
{
    battery_update();
    return battery_mv >= BATTERY_CHARGING_VOLTAGE_MV && battery_mv + BATTERY_CHARGING_DROP_MV > battery_peak_mv;
}
