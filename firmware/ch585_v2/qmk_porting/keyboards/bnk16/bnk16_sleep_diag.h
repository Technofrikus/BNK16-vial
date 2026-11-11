/*
 * BNK16 deep sleep diagnostics, see bnk16_sleep_diag.c. bnk16.c calls these hooks without any #ifdef:
 * in a normal build (none of the switches defined) they are empty and the compiler drops them.
 */
#pragma once

#include QMK_KEYBOARD_H

#if defined(BNK16_FAULT_DIAG) || defined(BNK16_BOOT_DIAG) || defined(BNK16_REGDUMP)
#define BNK16_SLEEP_DIAG
#endif

#ifdef BNK16_SLEEP_DIAG
void bnk16_sleep_diag_boot(void);                  /* first thing in main() */
void bnk16_sleep_diag_task(void);                  /* every housekeeping pass */
void bnk16_sleep_diag_scan(void);                  /* every matrix scan */
bool bnk16_sleep_diag_boot_color(rgb_t *color);    /* true while the boot colour is shown */
void bnk16_sleep_diag_feed(void);                  /* watchdog feed in loops that block on purpose */
void bnk16_sleep_diag_gpio_irq(void);              /* wake-up pin interrupt */
void bnk16_sleep_diag_ds_enter(void);              /* deep sleep entry, interrupts still on */
void bnk16_sleep_diag_ds_enter_irq_off(void);      /* deep sleep entry, interrupts off */
void bnk16_sleep_diag_ds_armed(void);              /* deep sleep entry, pins armed and USB off */
bool bnk16_sleep_diag_ds_poll(void);               /* deep sleep pass; true = end this deep sleep now */
void bnk16_sleep_diag_ds_exit(void);               /* deep sleep exit, pins still armed */
void bnk16_sleep_diag_ds_exit_done(void);          /* deep sleep exit finished */
void bnk16_sleep_diag_shutdown(void);              /* right before the chip is switched off */
bool bnk16_usb_is_connected(void);                 /* bnk16.c: USB enumerated and not suspended */
#else
static inline void bnk16_sleep_diag_boot(void) {}
static inline void bnk16_sleep_diag_task(void) {}
static inline void bnk16_sleep_diag_scan(void) {}
static inline bool bnk16_sleep_diag_boot_color(rgb_t *color) { (void)color; return false; }
static inline void bnk16_sleep_diag_feed(void) {}
static inline void bnk16_sleep_diag_gpio_irq(void) {}
static inline void bnk16_sleep_diag_ds_enter(void) {}
static inline void bnk16_sleep_diag_ds_enter_irq_off(void) {}
static inline void bnk16_sleep_diag_ds_armed(void) {}
static inline bool bnk16_sleep_diag_ds_poll(void) { return false; }
static inline void bnk16_sleep_diag_ds_exit(void) {}
static inline void bnk16_sleep_diag_ds_exit_done(void) {}
static inline void bnk16_sleep_diag_shutdown(void) {}
#endif
