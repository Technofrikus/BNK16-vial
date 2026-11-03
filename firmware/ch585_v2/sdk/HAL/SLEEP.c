/********************************** (C) COPYRIGHT *******************************
 * File Name          : SLEEP.c
 * Author             : WCH
 * Version            : V1.2
 * Date               : 2022/01/18
 * Description        : 睡眠配置及其初始化
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for 
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "HAL.h"
#include <stdbool.h>
#ifdef BNK16_SLEEP_DIAG
#define DIAG_CRUMB(x) (R8_GLOB_RESET_KEEP = (x)) /* see bnk16_diag.h */
#else
#define DIAG_CRUMB(x)
#endif

/*******************************************************************************
 * @fn          CH58X_LowPower
 *
 * @brief       启动睡眠
 *
 * @param   time    - 唤醒的时间点（RTC绝对值）
 *
 * @return      state.
 */
/* The application can veto sleep, e.g. while it needs its timers and peripherals running.
 * Called with interrupts disabled. */
__attribute__((weak)) bool hal_sleep_allowed(void)
{
    return true;
}

/* CH585 version, taken from the official CH585 EVT (EVT/EXAM/BLE/HAL/SLEEP.c): the CH585 BLE library has
 * no WakeUpTime setting, so the RTC is set to wake WAKE_UP_RTC_MAX_TIME early here for the 32 MHz crystal
 * to start. Interrupts stay disabled across the WFE sleep, so a pin interrupt that is already pending
 * ends the sleep at once instead of being lost. Added: the hal_sleep_allowed() veto. */
/* How CH58X_LowPower() ended, for diagnostics: HAL_SLEEP_STAT_x */
volatile uint32_t hal_sleep_stats[HAL_SLEEP_STAT_NUM];

__HIGH_CODE
uint32_t CH58X_LowPower(uint32_t time)
{
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    uint32_t time_tign, time_sleep, time_curr;
    unsigned long irq_status;

    /* wake up early */
    if (time <= WAKE_UP_RTC_MAX_TIME) {
        time_tign = time + (RTC_TIMER_MAX_VALUE - WAKE_UP_RTC_MAX_TIME);
    } else {
        time_tign = time - WAKE_UP_RTC_MAX_TIME;
    }

    SYS_DisableAllIrq(&irq_status);
    time_curr = RTC_GetCycle32k();
    if (time_tign < time_curr) {
        time_sleep = time_tign + (RTC_TIMER_MAX_VALUE - time_curr);
    } else {
        time_sleep = time_tign - time_curr;
    }

    /* too short or too long, or vetoed: no sleep */
    if ((time_sleep < SLEEP_RTC_MIN_TIME) || (time_sleep > SLEEP_RTC_MAX_TIME) || !hal_sleep_allowed()) {
        hal_sleep_stats[hal_sleep_allowed() ? HAL_SLEEP_STAT_SHORT : HAL_SLEEP_STAT_VETO]++;
        SYS_RecoverIrq(irq_status);
        return 2;
    }

    RTC_SetTignTime(time_tign);
    R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);

    if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG))
    {
        uint8_t x32Mpw;

        DIAG_CRUMB(0xB2);
#ifdef BNK16_SLEEP_HALT
        /* halt instead of sleep: clocks and flash stop, no power plan, the regulators stay as they are;
         * the crystal stops too until the early wake-up (the second halt below keeps it running) */
        sys_safe_access_enable();
        R8_HFCK_PWR_CTRL &= ~RB_CLK_XT32M_KEEP;
        sys_safe_access_disable();
        LowPower_Halt_WFE();
#else
        LowPower_Sleep_WFE(RB_PWR_RAM32K | RB_PWR_RAM96K | RB_PWR_EXTEND);
#endif

        /* 32 MHz crystal at 150 % bias current while it starts */
        x32Mpw = R8_XT32M_TUNE;
        x32Mpw = (x32Mpw & 0xfc) | 0x03;
        sys_safe_access_enable();
        R8_XT32M_TUNE = x32Mpw;
        sys_safe_access_disable();

        if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG)) /* woken by something else than the RTC, e.g. a pin */
        {
            mDelayuS(1400); /* the 32 MHz crystal is not stable yet */
            SetSysClock(Fsys);
            hal_sleep_stats[HAL_SLEEP_STAT_PIN]++;
            DIAG_CRUMB(0xB4);
            SYS_RecoverIrq(irq_status);
            return 0;
        }

        /* woken early by the RTC: halt until the real wake-up time, the crystal keeps running */
        R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
        RTC_SetTignTime(time);
        sys_safe_access_enable();
        R8_HFCK_PWR_CTRL |= RB_CLK_XT32M_KEEP;
        sys_safe_access_disable();
        if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG))
        {
            LowPower_Halt_WFE();
        }
        SetSysClock(Fsys);
        R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
        SYS_RecoverIrq(irq_status);
        HSECFG_Current(HSE_RCur_100); /* back to the rated bias current */
        hal_sleep_stats[HAL_SLEEP_STAT_RTC]++;
        DIAG_CRUMB(0xB3);
        return 0;
    }
    hal_sleep_stats[HAL_SLEEP_STAT_LATE]++;
    SYS_RecoverIrq(irq_status);
#endif
    return 3;
}

/*******************************************************************************
 * @fn      HAL_SleepInit
 *
 * @brief   配置睡眠唤醒的方式   - RTC唤醒，触发模式
 *
 * @param   None.
 *
 * @return  None.
 */
void HAL_SleepInit(void)
{
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    sys_safe_access_enable();
    R8_SLP_WAKE_CTRL |= RB_SLP_RTC_WAKE; // RTC唤醒
    sys_safe_access_disable();              //
    sys_safe_access_enable();
    R8_RTC_MODE_CTRL |= RB_RTC_TRIG_EN;  // 触发模式
    sys_safe_access_disable();              //
    PFIC_EnableIRQ(RTC_IRQn);
#endif
}
