#pragma once
#include <stdint.h>

/* Non-blocking haptic patterns (TMOS task), see haptic.c. Call haptic_init() after CH58x_BLEInit(). */
void haptic_init(void);
/* Appends one burst of `pushes` pushes, then a pause; bursts play in order */
void haptic_queue(uint8_t pushes, uint16_t pause_after_ms);
