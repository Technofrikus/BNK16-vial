/* BNK16 bring-up diagnostics, see bnk16_diag.c */
#pragma once

#ifdef BNK16_INTERVAL_DEBUG
void bnk16_diag_note_key(void);
#endif

#ifdef BNK16_SLEEP_DIAG
/* Breadcrumbs in R8_GLOB_RESET_KEEP, which survives every reset except a real power loss */
#define DIAG_CRUMB(x) (R8_GLOB_RESET_KEEP = (x))
#define DIAG_CRUMB_BOOT       0xB0
#define DIAG_CRUMB_DEEP_ENTER 0xB1 /* pins armed, deep sleep loop running */
#define DIAG_CRUMB_SLEEP      0xB2 /* inside CH58X_LowPower, about to sleep */
#define DIAG_CRUMB_WOKE_RTC   0xB3
#define DIAG_CRUMB_WOKE_PIN   0xB4
#define DIAG_CRUMB_DEEP_EXIT  0xB5 /* pins back to QMK */
#define DIAG_CRUMB_SHUTDOWN   0xB6 /* wake-up pins armed, about to shut down */
#define DIAG_CRUMB_SHUTDOWN_ABORT 0xB7 /* shutdown skipped: a key or knob moved while the pins were armed */
void bnk16_diag_boot(void);
#else
#define DIAG_CRUMB(x)
static inline void bnk16_diag_boot(void) {}
#endif

#if defined(BNK16_PIN_SCAN) || defined(BNK16_SWITCH_PROBE) || defined(BNK16_INTERVAL_DEBUG) || defined(BNK16_SLEEP_DIAG) || defined(BNK16_VBUS_PROBE)
void bnk16_diag_task(void);
#else
static inline void bnk16_diag_task(void) {}
#endif
