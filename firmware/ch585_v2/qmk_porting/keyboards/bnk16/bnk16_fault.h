/*
 * BNK16_FAULT_DIAG (test builds, phase 47g): finds out whether the "reboot" is a crash.
 * The SDK's HardFault_Handler (CH58x_sys.c) resets in a way that reports as a power-on reset, so a crash
 * looked like a power loss (white boot colour, keep register wiped). With this switch our own handler
 * stores the fault registers in a RAM block outside everything the firmware and the bootloader use, then
 * does a plain software reset, which keeps the RAM. The next boot reads the block and types it.
 * Breadcrumbs: BNK16_STAGE(x) records how far the deep sleep got; the block is valid only with its magic.
 * Used from bnk16.c and sdk/HAL/SLEEP.c (C only, no QMK headers).
 */
#pragma once

#include <stdint.h>

#ifdef BNK16_FAULT_DIAG
typedef struct {
    uint32_t magic;  /* FAULT_DIAG_MAGIC while the block is valid */
    uint32_t fault;  /* 0 = no fault, 1 = hard fault/exception, 2 = NMI */
    uint32_t mcause;
    uint32_t mepc;
    uint32_t mtval;
    uint32_t sp;     /* stack pointer inside the handler */
    uint32_t stage;  /* last BNK16_STAGE() value */
    uint32_t sleeps; /* chip sleeps entered since boot */
    uint32_t wakes;  /* chip sleeps returned from since boot */
    /* phase 47i: how CH58X_LowPower() calls ended while sleep was allowed */
    uint32_t calls;  /* calls with the sleep allowed */
    uint32_t shorts; /* too short or too long, no sleep (2) */
    uint32_t lates;  /* RTC time already passed, no sleep (3) */
    uint32_t pins;   /* woken by something else than the RTC */
    uint32_t loops;  /* deep sleep passes of the main loop */
    uint32_t skips;  /* halt skipped: the real wake-up time was too close or had passed (phase 47j) */
    /* phase 47l: interrupt entries, and what the soft watchdog (TMR3) saw when it fired */
    uint32_t isr_rtc, isr_gpio, isr_bb;
    uint32_t fed;     /* feed counter, the soft watchdog checks that it moves */
    uint32_t gisr;    /* PFIC->GISR at the soft watchdog: active/nesting state */
    uint32_t ipr0, ipr1; /* PFIC pending bits at the soft watchdog */
    uint32_t ien0, ien1; /* PFIC enable bits at the soft watchdog */
    /* phase 47n: USB HS interrupt entries and the last R8_USB2_INT_FG seen there */
    uint32_t isr_usb, usb_flag;
} bnk16_fault_t;

#define FAULT_DIAG_MAGIC 0x5EEDB16Bu
/* 16 KB below the top of RAM: the stacks of the firmware and the bootloader start at the top, the
 * firmware's data ends near 0x2000A000 and the bootloader's near 0x20001700 */
#define BNK16_FAULT ((volatile bnk16_fault_t *)0x2001C000)

#define BNK16_STAGE(x) (BNK16_FAULT->stage = (x))
#define BNK16_SLEEP_COUNT() (BNK16_FAULT->sleeps++)
#define BNK16_WAKE_COUNT() (BNK16_FAULT->wakes++)
#define BNK16_COUNT(f) (BNK16_FAULT->f++)
#define BNK16_SET(f, v) (BNK16_FAULT->f = (v))

/* fault codes: 1 = exception, 2 = NMI, 3 = soft watchdog (TMR3) caught a hang */

/* Stage codes */
#define STAGE_AWAKE        0x01 /* main loop running, not in deep sleep */
#define STAGE_DS_ENTER     0x10 /* bnk16_deep_sleep_enter() started */
#define STAGE_DS_ARMED     0x11 /* pins armed, USB off */
#define STAGE_DS_LOOP      0x12 /* deep sleep pass of the main loop */
#define STAGE_DS_EXIT      0x13 /* bnk16_deep_sleep_exit() started */
#define STAGE_DS_EXIT_USB  0x14 /* before USBHS_Device_Init */
#define STAGE_DS_EXIT_DONE 0x15
#define STAGE_LP_ENTER     0x20 /* CH58X_LowPower: allowed, about to sleep */
#define STAGE_LP_WOKE      0x21 /* back from LowPower_Sleep_WFE */
#define STAGE_LP_PIN       0x22 /* woken by a pin, before SetSysClock */
#define STAGE_LP_HALT      0x23 /* RTC early wake, before the halt */
#define STAGE_LP_CLOCK     0x24 /* after SetSysClock */
#define STAGE_LP_DONE      0x25 /* interrupts back on, returning */
#else
#define BNK16_STAGE(x)
#define BNK16_SLEEP_COUNT()
#define BNK16_WAKE_COUNT()
#define BNK16_COUNT(f)
#define BNK16_SET(f, v)
#endif
