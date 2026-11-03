/* BNK16 bring-up diagnostics, see bnk16_diag.c */
#pragma once

#ifdef BNK16_INTERVAL_DEBUG
void bnk16_diag_note_key(void);
#endif

#if defined(BNK16_PIN_SCAN) || defined(BNK16_SWITCH_PROBE) || defined(BNK16_INTERVAL_DEBUG)
void bnk16_diag_task(void);
#else
static inline void bnk16_diag_task(void) {}
#endif
