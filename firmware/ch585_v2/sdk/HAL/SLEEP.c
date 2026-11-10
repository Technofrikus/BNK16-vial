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
#if __has_include("bnk16_fault.h")
#include "bnk16_fault.h" /* BNK16_STAGE() breadcrumbs, empty unless BNK16_FAULT_DIAG */
#else
#define BNK16_STAGE(x)
#define BNK16_SLEEP_COUNT()
#define BNK16_WAKE_COUNT()
#define BNK16_COUNT(f)
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
/* The application can veto the sleep, e.g. while it needs its timers and peripherals running (BNK16: only
 * the deep sleep in power_management.c allows it). Called with interrupts disabled. */
__attribute__((weak)) bool hal_sleep_allowed(void)
{
    return true;
}

/* CH585 version from the newest WCH EVT (website zip, CH585_List_EN.txt 2026.10, EXAM/BLE/HAL/SLEEP.c,
 * which comes with BLE library V1.5.0928). The CH585 library has no WakeUpTime setting, so the RTC is set
 * to wake WAKE_UP_RTC_MAX_TIME early here for the 32 MHz crystal. Interrupts stay disabled across the WFE
 * sleep, so a pin interrupt that is already pending ends the sleep at once instead of being lost.
 * Two paths, as in the EVT: newer chip revisions (chip_info bits 24-27) let the hardware start the crystal
 * early (RB_XT_PRE_EN); older ones wake by the RTC early, then halt with the crystal running until the
 * real time. The older path is the one of the GitHub EVT (openwch/ch585 db88919), which held the link
 * on this board in test B; the website version raises the crystal current before the sleep instead of
 * after it and lowers it again after a pin wake-up. HAL_SLEEP_XT_PRE 0 forces the older path.
 * BNK16 changes: the hal_sleep_allowed() veto, no UART wait, Fsys for SYSCLK_FREQ, and SLEEPDEEP cleared
 * after every wake-up (below). */
/* LowPower_Sleep_WFE() and LowPower_Halt_WFE() set SLEEPDEEP (PFIC->SCTLR bit 2) and leave it set; only
 * LowPower_Idle() clears it. WCH's examples never execute WFI outside these functions, but our main loop
 * does (__WFI() in main() and platform_run()): with the bit still set, that WFI is a deep sleep without
 * any of the preparation (clock still on the PLL, flash not set up, no power plan), and the board lost
 * power on the first chip sleep (phase 38e, 47c/47d; register snapshot of test B, phase 47e). */
#define SLEEPDEEP_CLEAR() (PFIC->SCTLR &= ~(1 << 2))
/* BNK16: least time left before the real wake-up for the halt in the RTC early-wake path (RTC ticks of
 * about 31 us); with less, or with the time already past, the halt is skipped */
#define HALT_MIN_TICKS 4

#ifndef HAL_SLEEP_XT_PRE
#define HAL_SLEEP_XT_PRE 1
#endif

__HIGH_CODE
uint32_t CH58X_LowPower(uint32_t time)
{
#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    uint32_t time_tign, time_sleep, time_curr;
    unsigned long irq_status;

    /* wake up early */
    if (time <= WAKE_UP_RTC_MAX_TIME) {
        time_tign = time + (RTC_MAX_COUNT - WAKE_UP_RTC_MAX_TIME);
    } else {
        time_tign = time - WAKE_UP_RTC_MAX_TIME;
    }

    SYS_DisableAllIrq(&irq_status);
    time_curr = RTC_GetCycle32k();
    if (time_tign < time_curr) {
        time_sleep = time_tign + (RTC_MAX_COUNT - time_curr);
    } else {
        time_sleep = time_tign - time_curr;
    }

    /* too short or too long, or vetoed: no sleep */
    if (!hal_sleep_allowed()) {
        SYS_RecoverIrq(irq_status);
        return 2;
    }
    BNK16_COUNT(calls);
    if ((time_sleep < SLEEP_RTC_MIN_TIME) || (time_sleep > SLEEP_RTC_MAX_TIME)) {
        BNK16_COUNT(shorts);
        SYS_RecoverIrq(irq_status);
        return 2;
    }

    RTC_SetTignTime(time_tign);
    R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);

    if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG))
    {
        uint8_t x32Mpw;

        BNK16_STAGE(STAGE_LP_ENTER);
        BNK16_SLEEP_COUNT();

        /* 32 MHz crystal at 150 % of its rated current while it starts */
        x32Mpw = R8_XT32M_TUNE;
        x32Mpw = (x32Mpw & 0xfc) | 0x03;
        sys_safe_access_enable();
        R8_XT32M_TUNE = x32Mpw;
        sys_safe_access_disable();

        if(HAL_SLEEP_XT_PRE && (chip_info & 0x0F000000))
        {
            LowPower_Sleep_WFE(RB_PWR_RAM32K | RB_PWR_RAM96K | RB_PWR_EXTEND | RB_XT_PRE_EN);
        }
        else
        {
            LowPower_Sleep_WFE(RB_PWR_RAM32K | RB_PWR_RAM96K | RB_PWR_EXTEND);
            BNK16_STAGE(STAGE_LP_WOKE);

            if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG)) /* woken by something else than the RTC, e.g. a pin */
            {
                BNK16_STAGE(STAGE_LP_PIN);
                BNK16_COUNT(pins);
                DelayUs(1400); /* the 32 MHz crystal is not stable yet */
                SetSysClock(Fsys);
                SLEEPDEEP_CLEAR();
                BNK16_STAGE(STAGE_LP_CLOCK);
                BNK16_WAKE_COUNT();
                SYS_RecoverIrq(irq_status);
                HSECFG_Current(HSE_RCur_100); /* back to the rated current */
                return 0;
            }

            /* woken early by the RTC: halt until the real wake-up time, the crystal keeps running */
            R8_RTC_FLAG_CTRL = (RB_RTC_TMR_CLR | RB_RTC_TRIG_CLR);
            RTC_SetTignTime(time);
            sys_safe_access_enable();
            R8_HFCK_PWR_CTRL |= RB_CLK_XT32M_KEEP;
            sys_safe_access_disable();
            /* BNK16: the RTC trigger fires only when the counter reaches `time`; if that is already past or
             * about to pass, the halt would never end (only interrupts could, and all are masked here),
             * as in phase 38g. Skip the halt then; TMOS waits the rest itself. */
            time_curr = RTC_GetCycle32k();
            time_sleep = (time >= time_curr) ? time - time_curr : time + (RTC_MAX_COUNT - time_curr);
            if(time_sleep < HALT_MIN_TICKS || time_sleep > WAKE_UP_RTC_MAX_TIME + HALT_MIN_TICKS)
            {
                BNK16_COUNT(skips);
            }
            else if(!(R8_RTC_FLAG_CTRL & RB_RTC_TRIG_FLAG))
            {
                BNK16_STAGE(STAGE_LP_HALT);
                LowPower_Halt_WFE();
            }
        }
        BNK16_STAGE(STAGE_LP_WOKE);
        SetSysClock(Fsys);
        SLEEPDEEP_CLEAR();
        BNK16_STAGE(STAGE_LP_CLOCK);
        BNK16_WAKE_COUNT();
        SYS_RecoverIrq(irq_status);
        HSECFG_Current(HSE_RCur_100); /* back to the rated current */
        BNK16_STAGE(STAGE_LP_DONE);
        return 0;
    }
    BNK16_COUNT(lates);
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
