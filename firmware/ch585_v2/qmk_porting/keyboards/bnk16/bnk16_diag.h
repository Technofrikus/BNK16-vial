/* BNK16 bring-up diagnostics, see bnk16_diag.c */
#pragma once

#if defined(BNK16_PIN_SCAN) || defined(BNK16_SWITCH_PROBE)
void bnk16_diag_task(void);
#else
static inline void bnk16_diag_task(void) {}
#endif
