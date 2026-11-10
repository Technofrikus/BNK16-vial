/*
 * BNK16 sleep test (test A): can the CH585 on this board sleep and wake up again, without BLE and QMK?
 *
 * Phase 38e found that the chip lost power on its first real sleep (keep register wiped), and in
 * phase 38g the halt never woke up. This firmware does only that: sleep, wake on the RTC, buzz.
 * Each variant (SLEEPTEST_MODE, see CMakeLists.txt) uses a different SDK sleep routine.
 *
 * Feedback on the haptic motor (PA7, haptic switch on):
 *   boot:   N long buzzes, N = reset flag + 1:  1 software reset, 2 real power on, 3 watchdog,
 *           4 reset pin, 6 woken from shutdown, 5/7/8 = power on reset during sleep (bad)
 *   wake:   one short buzz every SLEEPTEST_PERIOD_S; every 10th wake-up one long buzz instead,
 *           so a RAM counter survived 10 sleeps
 * A reboot shows as the boot pattern again. Nothing at all after the boot pattern = it never woke.
 *
 * Back to the normal firmware: slide switch off, hold the top-left key, switch on, drop the UF2
 * onto BNK16BOOT.
 */

#include "bnk16_test.h"

#define MODE_IDLE          1 /* control: WFI without deep sleep, must always work */
#define MODE_SLEEP         2 /* LowPower_Sleep, all RAM + extend kept, RTC interrupt wakes (WFI) */
#define MODE_SLEEP_NOEXT   3 /* as MODE_SLEEP without RB_PWR_EXTEND (USB/BLE retention area) */
#define MODE_SLEEP_WFE     4 /* the BLE HAL path of phase 38: interrupts off, LowPower_Sleep_WFE */
#define MODE_SLEEP_WFE_SEV 5 /* as MODE_SLEEP_WFE, plus SEVONPEND (pending interrupts wake WFE) */
#define MODE_HALT          6 /* phase 38g: LowPower_Halt_WFE, crystal off */
#define MODE_HALT_SEV      7 /* as MODE_HALT, plus SEVONPEND */
#define MODE_SHUTDOWN      8 /* baseline: LowPower_Shutdown with no wake-up source, stays off until a power cycle */

/* Measuring builds (SLEEPTEST_PINS): the pins are set the way bnk16_shutdown() leaves them, so the
 * sleep current compares with the 61 uA shutdown of phase 39c. */
#ifndef SLEEPTEST_PINS
#define SLEEPTEST_PINS 0
#endif

#ifndef SLEEPTEST_MODE
#error "SLEEPTEST_MODE not set"
#endif
#ifndef SLEEPTEST_PERIOD_S
#define SLEEPTEST_PERIOD_S 5
#endif

#define SYS_CLK     CLK_SOURCE_HSE_PLL_62_4MHz /* same as the keyboard firmware */
#define LSI_HZ      32000
#define RAM_ALL     (RB_PWR_RAM32K | RB_PWR_RAM96K)

static volatile uint32_t wake_count;
static volatile uint32_t rtc_irqs;

__INTERRUPT __HIGH_CODE void RTC_IRQHandler(void) {
    R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
    rtc_irqs++;
}

static void rtc_init(void) {
    /* internal 32 kHz RC, as the keyboard firmware (CLK_OSC32K, no 32 kHz crystal on this board) */
    sys_safe_access_enable();
    R8_CK32K_CONFIG &= ~(RB_CLK_OSC32K_XT | RB_CLK_XT32K_PON);
    sys_safe_access_disable();
    sys_safe_access_enable();
    R8_CK32K_CONFIG |= RB_CLK_INT32K_PON;
    sys_safe_access_disable();
    Calibration_LSI(Level_64);
    RTC_InitTime(2020, 1, 1, 0, 0, 0);

    /* RTC may wake the chip from sleep (HAL_SleepInit does the same) */
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_RTC_WAKE;
    sys_safe_access_disable();
    R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
    PFIC_EnableIRQ(RTC_IRQn);
}

/* Sleeps until the RTC fires. Runs from RAM: flash is off in some of the sleep modes. */
__HIGH_CODE static void sleep_once(void) {
    RTC_TRIGFunCfg(SLEEPTEST_PERIOD_S * LSI_HZ);

#if SLEEPTEST_MODE == MODE_IDLE
    LowPower_Idle();

#elif SLEEPTEST_MODE == MODE_SLEEP || SLEEPTEST_MODE == MODE_SLEEP_NOEXT
    /* WFI with the RTC interrupt enabled; LowPower_Sleep restores clock and flash itself */
#if SLEEPTEST_MODE == MODE_SLEEP
    LowPower_Sleep(RAM_ALL | RB_PWR_EXTEND);
#else
    LowPower_Sleep(RAM_ALL);
#endif
    mDelayuS(300); /* SDK: wait 300 us before running from flash again */
    HSECFG_Current(HSE_RCur_100);

#else
    /* WFE variants, the way the BLE HAL (sdk/HAL/SLEEP.c, phase 38) sleeps: interrupts off, so the
     * pending RTC interrupt only ends the sleep and is served after SYS_RecoverIrq */
    uint32_t      irq_status;
    uint8_t       x32Mpw;
#if SLEEPTEST_MODE == MODE_SLEEP_WFE_SEV || SLEEPTEST_MODE == MODE_HALT_SEV
    PFIC->SCTLR |= (1 << 4); /* SEVONPEND */
#endif
    SYS_DisableAllIrq(&irq_status);
#if SLEEPTEST_MODE == MODE_SLEEP_WFE || SLEEPTEST_MODE == MODE_SLEEP_WFE_SEV
    LowPower_Sleep_WFE(RAM_ALL | RB_PWR_EXTEND);
#else
    sys_safe_access_enable();
    R8_HFCK_PWR_CTRL &= ~RB_CLK_XT32M_KEEP; /* crystal stops too, as in phase 38g */
    sys_safe_access_disable();
    LowPower_Halt_WFE();
#endif
    /* 32 MHz crystal at 150 % bias while it starts, then wait for it and restore the clock */
    x32Mpw = R8_XT32M_TUNE;
    x32Mpw = (x32Mpw & 0xfc) | 0x03;
    sys_safe_access_enable();
    R8_XT32M_TUNE = x32Mpw;
    sys_safe_access_disable();
    mDelayuS(1400);
    SetSysClock(SYS_CLK);
    HSECFG_Current(HSE_RCur_100);
    SYS_RecoverIrq(irq_status);
#endif
}

int main(void) {
    uint8_t reset_flag = R8_RESET_STATUS & RB_RESET_FLAG;

    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYS_CLK);
    pins_board_init();

    mDelaymS(500);
    for (uint8_t i = 0; i <= reset_flag; i++) {
        buzz(BUZZ_LONG);
        mDelaymS(400);
    }

#if SLEEPTEST_PINS
    pins_measure_init();
#endif
#if SLEEPTEST_MODE == MODE_SHUTDOWN
    mDelaymS(1000);
    uint32_t irq_status;
    SYS_DisableAllIrq(&irq_status);
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL = 0; /* no wake-up source */
    sys_safe_access_disable();
    LowPower_Shutdown(0);
    while (1) {
    }
#endif

    rtc_init();
    mDelaymS(1000);

    while (1) {
        sleep_once();
        wake_count++;
        buzz(wake_count % 10 == 0 ? BUZZ_LONG : BUZZ_SHORT);
    }
}
