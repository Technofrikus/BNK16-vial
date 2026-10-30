#include "quantum.h"
#include "ws2812.h"
#include "ws2812_supplement.h"

/* Adapted from https://github.com/joewa/WS2812-LED-Driver_ChibiOS/ */

#ifdef WS2812_RGBW
#define WS2812_CHANNELS 4
#else
#define WS2812_CHANNELS 3
#endif

#ifndef RGBLED_NUM
#define RGBLED_NUM WS2812_LED_COUNT
#endif

static volatile bool ws2812_frame_busy = false; /**< true from flush until the DMA frame has been sent */

#ifndef WS2812_PWM_DRIVER
#define WS2812_PWM_DRIVER 1 // TMR1
#define WS2812_DI_PIN     A10
#endif

#if WS2812_PWM_DRIVER == 1
#define WS2812_PWM_CNT_END_REG R32_TMR1_CNT_END
#define WS2812_DMA_STOP R8_TMR1_CTRL_DMA = 0
#define WS2812_DMA_CONFIG(en, start, end)       \
    TMR1_DMACfg(en, (uint32_t)&start, \
                (uint32_t)&end, Mode_Single)
/* Counter stopped, counter and FIFO cleared, PWM output on (line low until the first pulse) */
#define WS2812_PWM_PREPARE(level)           \
    do {                                    \
        TMR1_PWMInit(level, PWM_Times_1);  \
        TMR1_PWMEnable();                  \
    } while (0);
#define WS2812_PWM_START TMR1_Enable();
#define WS2812_PWM_DMA_INTERRUPT_ENABLE PFIC_EnableIRQ(TMR1_IRQn);
#define WS2812_PWM_DMA_INTERRUPT_SET    do { TMR1_ClearITFlag(RB_TMR_IF_DMA_END); TMR1_ITCfg(ENABLE, RB_TMR_IE_DMA_END); } while (0);
#define WS2812_PWM_CLOCK_ON                                  \
    do {                                                     \
        sys_safe_access_enable();                            \
        R8_SLP_CLK_OFF0 &= ~RB_SLP_CLK_TMR1;                 \
        sys_safe_access_disable();                           \
    } while (R8_SLP_CLK_OFF0 & RB_SLP_CLK_TMR1);

/* Single-shot DMA: the frame goes out exactly once. Afterwards the timer keeps running with the
 * last (zero duty, reset) value, so the data line stays low. Gating the timer clock here instead
 * could freeze it mid-bit when the interrupt comes late and garble the next frame. */
__INTERRUPT __HIGH_CODE void TMR1_IRQHandler()
{
    R8_TMR1_INTER_EN = 0;
    TMR1_ClearITFlag(RB_TMR_IF_DMA_END);
    ws2812_frame_busy = false;
}
#elif WS2812_PWM_DRIVER == 2
#define WS2812_PWM_CNT_END_REG R32_TMR2_CNT_END
#define WS2812_DMA_STOP R8_TMR2_CTRL_DMA = 0
#define WS2812_DMA_CONFIG(en, start, end)       \
    TMR2_DMACfg(en, (uint32_t)&start, \
                (uint32_t)&end, Mode_Single)
/* Counter stopped, counter and FIFO cleared, PWM output on (line low until the first pulse) */
#define WS2812_PWM_PREPARE(level)           \
    do {                                    \
        TMR2_PWMInit(level, PWM_Times_1);  \
        TMR2_PWMEnable();                  \
    } while (0);
#define WS2812_PWM_START TMR2_Enable();
#define WS2812_PWM_DMA_INTERRUPT_ENABLE PFIC_EnableIRQ(TMR2_IRQn);
#define WS2812_PWM_DMA_INTERRUPT_SET    do { TMR2_ClearITFlag(RB_TMR_IF_DMA_END); TMR2_ITCfg(ENABLE, RB_TMR_IE_DMA_END); } while (0);
#define WS2812_PWM_CLOCK_ON                                  \
    do {                                                     \
        sys_safe_access_enable();                            \
        R8_SLP_CLK_OFF0 &= ~RB_SLP_CLK_TMR2;                 \
        sys_safe_access_disable();                           \
    } while (R8_SLP_CLK_OFF0 & RB_SLP_CLK_TMR2);

__INTERRUPT __HIGH_CODE void TMR2_IRQHandler()
{
    R8_TMR2_INTER_EN = 0;
    TMR2_ClearITFlag(RB_TMR_IF_DMA_END);
    ws2812_frame_busy = false;
}
#else
// Only TMR1 and TMR2 support DMA and PWM
// Only pins A10, A11. B10, B11 is used for USB.
#error Unsupported PWM Driver.
#endif

#ifndef WS2812_PWM_TARGET_PERIOD
// #    define WS2812_PWM_TARGET_PERIOD 800000 // Original code is 800k...?
#define WS2812_PWM_TARGET_PERIOD 80000 // TODO: work out why 10x less on f303/f4x1
#endif

/* --- PRIVATE CONSTANTS ---------------------------------------------------- */

#define WS2812_PWM_FREQUENCY FREQ_SYS                                          /**< Clock frequency of PWM, must be valid with respect to system clock! */
#define WS2812_PWM_PERIOD    (WS2812_PWM_FREQUENCY / WS2812_PWM_TARGET_PERIOD) /**< Clock period in ticks. 1 / 800kHz = 1.25 uS (as per datasheet) */

/**
 * @brief   Number of bit-periods to hold the data line low at the end of a frame
 *
 * The reset period for each frame is defined in WS2812_TRST_US.
 * Calculate the number of zeroes to add at the end assuming 1.25 uS/bit:
 */
#define WS2812_LEAD_BIT_N  2 /**< Zero-duty periods before the data, absorb the first (possibly partial) timer period */
#define WS2812_COLOR_BITS  (WS2812_CHANNELS * 8)
#define WS2812_RESET_BIT_N (1000 * WS2812_TRST_US / WS2812_TIMING)
#define WS2812_COLOR_BIT_N (RGBLED_NUM * WS2812_COLOR_BITS)          /**< Number of data bits */
#define WS2812_BIT_N       (WS2812_LEAD_BIT_N + WS2812_COLOR_BIT_N + WS2812_RESET_BIT_N) /**< Total number of periods in a frame */

/**
 * @brief   High period for a zero, in ticks
 *
 * Per the datasheet:
 * WS2812:
 * - T0H: 200 nS to 500 nS, inclusive
 * - T0L: 650 nS to 950 nS, inclusive
 * WS2812B:
 * - T0H: 200 nS to 500 nS, inclusive
 * - T0L: 750 nS to 1050 nS, inclusive
 *
 * The duty cycle is calculated for a high period of 350 nS.
 */
#define WS2812_DUTYCYCLE_0 (WS2812_PWM_FREQUENCY / (1000000000 / 350))

/**
 * @brief   High period for a one, in ticks
 *
 * Per the datasheet:
 * WS2812:
 * - T1H: 550 nS to 850 nS, inclusive
 * - T1L: 450 nS to 750 nS, inclusive
 * WS2812B:
 * - T1H: 750 nS to 1050 nS, inclusive
 * - T1L: 200 nS to 500 nS, inclusive
 *
 * The duty cycle is calculated for a high period of 800 nS.
 * This is in the middle of the specifications of the WS2812 and WS2812B.
 */
#define WS2812_DUTYCYCLE_1 (WS2812_PWM_FREQUENCY / (1000000000 / 800))

/* --- PRIVATE MACROS ------------------------------------------------------- */

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given bit
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] byte:                 The byte number [0, 2]
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_BIT(led, byte, bit) (WS2812_LEAD_BIT_N + WS2812_COLOR_BITS * (led) + 8 * (byte) + (7 - (bit)))

#if (WS2812_BYTE_ORDER == WS2812_BYTE_ORDER_GRB)
/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given red bit
 *
 * @note    The red byte is the middle byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_RED_BIT(led, bit) WS2812_BIT((led), 1, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given green bit
 *
 * @note    The red byte is the first byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_GREEN_BIT(led, bit) WS2812_BIT((led), 0, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given blue bit
 *
 * @note    The red byte is the last byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit index [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_BLUE_BIT(led, bit) WS2812_BIT((led), 2, (bit))

#elif (WS2812_BYTE_ORDER == WS2812_BYTE_ORDER_RGB)
/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given red bit
 *
 * @note    The red byte is the middle byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_RED_BIT(led, bit)   WS2812_BIT((led), 0, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given green bit
 *
 * @note    The red byte is the first byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_GREEN_BIT(led, bit) WS2812_BIT((led), 1, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given blue bit
 *
 * @note    The red byte is the last byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit index [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_BLUE_BIT(led, bit)  WS2812_BIT((led), 2, (bit))

#elif (WS2812_BYTE_ORDER == WS2812_BYTE_ORDER_BGR)
/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given red bit
 *
 * @note    The red byte is the middle byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_RED_BIT(led, bit)   WS2812_BIT((led), 2, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given green bit
 *
 * @note    The red byte is the first byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit number [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_GREEN_BIT(led, bit) WS2812_BIT((led), 1, (bit))

/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given blue bit
 *
 * @note    The red byte is the last byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref RGBLED_NUM)
 * @param[in] bit:                  The bit index [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_BLUE_BIT(led, bit)  WS2812_BIT((led), 0, (bit))
#endif

#ifdef WS2812_RGBW
/**
 * @brief   Determine the index in @ref ws2812_frame_buffer "the frame buffer" of a given white bit
 *
 * @note    The white byte is the last byte in the color packet
 *
 * @param[in] led:                  The led index [0, @ref WS2812_LED_N)
 * @param[in] bit:                  The bit index [0, 7]
 *
 * @return                          The bit index
 */
#define WS2812_WHITE_BIT(led, bit) WS2812_BIT((led), 3, (bit))
#endif

/* --- PRIVATE VARIABLES ---------------------------------------------------- */

static uint32_t ws2812_frame_buffer[WS2812_BIT_N + 1]; /**< Buffer for a frame */

/* --- PUBLIC FUNCTIONS ----------------------------------------------------- */
/*
 * Gedanke: Double-buffer type transactions: double buffer transfers using two memory pointers for
the memory (while the DMA is reading/writing from/to a buffer, the application can
write/read to/from the other buffer).
 */

/*
 * Start sending the frame buffer. The timer is stopped and cleared first and only started once the
 * DMA is set up, so every frame begins at the start of a timer period. Restarting the DMA on a
 * running timer can cost the first data bit its pulse: the frame then shifts one bit left and every
 * LED flashes at about twice its brightness. The zero-duty lead-in covers the first period.
 */
static void ws2812_start_frame(void)
{
    ws2812_frame_busy = true;
    WS2812_DMA_STOP;
    WS2812_PWM_PREPARE(High_Level);
    WS2812_PWM_CNT_END_REG = WS2812_PWM_PERIOD;
    WS2812_PWM_DMA_INTERRUPT_SET;
    WS2812_DMA_CONFIG(ENABLE, ws2812_frame_buffer[0], ws2812_frame_buffer[WS2812_BIT_N + 1]);
    WS2812_PWM_START;
}

void ws2812_init(void)
{
    // Initialize led frame buffer
    uint32_t i;

    for (i = 0; i < WS2812_LEAD_BIT_N; i++)
        ws2812_frame_buffer[i] = 0; // Lead-in, line stays low
    for (i = 0; i < WS2812_COLOR_BIT_N; i++)
        ws2812_frame_buffer[i + WS2812_LEAD_BIT_N] = WS2812_DUTYCYCLE_0; // All color bits are zero duty cycle
    for (i = 0; i < WS2812_RESET_BIT_N; i++)
        ws2812_frame_buffer[i + WS2812_LEAD_BIT_N + WS2812_COLOR_BIT_N] = 0; // All reset bits are zero

    // The pin falls back to its GPIO level while the timer is cleared between frames, so keep that low
    gpio_write_pin_low(WS2812_DI_PIN);
    gpio_set_pin_output(WS2812_DI_PIN);
#ifdef WS2812_DI_HIGH_DRIVE
    GPIOA_ModeCfg(WS2812_DI_PIN, GPIO_ModeOut_PP_20mA);
#endif

    WS2812_PWM_CLOCK_ON;
    // Send one all-zero frame; the timer then keeps running with the line held low
    ws2812_start_frame();
    WS2812_PWM_DMA_INTERRUPT_ENABLE;
}

static ws2812_led_t ws2812_leds[WS2812_LED_COUNT];

static void ws2812_write_led(uint16_t led_number, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    // Write color to frame buffer
    for (uint8_t bit = 0; bit < 8; bit++) {
        ws2812_frame_buffer[WS2812_RED_BIT(led_number, bit)] = ((r >> bit) & 0x01) ? WS2812_DUTYCYCLE_1 : WS2812_DUTYCYCLE_0;
        ws2812_frame_buffer[WS2812_GREEN_BIT(led_number, bit)] = ((g >> bit) & 0x01) ? WS2812_DUTYCYCLE_1 : WS2812_DUTYCYCLE_0;
        ws2812_frame_buffer[WS2812_BLUE_BIT(led_number, bit)] = ((b >> bit) & 0x01) ? WS2812_DUTYCYCLE_1 : WS2812_DUTYCYCLE_0;
#ifdef WS2812_RGBW
        ws2812_frame_buffer[WS2812_WHITE_BIT(led_number, bit)] = ((w >> bit) & 0x01) ? WS2812_DUTYCYCLE_1 : WS2812_DUTYCYCLE_0;
#endif
    }
}

void ws2812_set_color(int index, uint8_t red, uint8_t green, uint8_t blue)
{
    ws2812_leds[index].r = red;
    ws2812_leds[index].g = green;
    ws2812_leds[index].b = blue;
#ifdef WS2812_RGBW
    ws2812_rgb_to_rgbw(&ws2812_leds[index]);
#endif
}

void ws2812_set_color_all(uint8_t red, uint8_t green, uint8_t blue)
{
    for (int i = 0; i < WS2812_LED_COUNT; i++) {
        ws2812_set_color(i, red, green, blue);
    }
}

void ws2812_flush(void)
{
    if (!ws2812_power_get()) {
        ws2812_power_toggle(true);
    }

    // The previous frame must be out before the buffer is touched. If its end interrupt got lost,
    // carry on after the timeout: the single-shot DMA has stopped by then anyway.
    uint16_t timeout_timer = timer_read();
    while (ws2812_frame_busy && timer_elapsed(timeout_timer) < WS2812_TIMEOUT) {
    }

    for (uint16_t i = 0; i < WS2812_LED_COUNT; i++) {
#ifdef WS2812_RGBW
        ws2812_write_led(i, ws2812_leds[i].r, ws2812_leds[i].g, ws2812_leds[i].b, ws2812_leds[i].w);
#else
        ws2812_write_led(i, ws2812_leds[i].r, ws2812_leds[i].g, ws2812_leds[i].b, 0);
#endif
    }

    ws2812_start_frame();
}
