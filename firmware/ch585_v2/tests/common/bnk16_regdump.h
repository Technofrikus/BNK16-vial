/*
 * Register snapshot for comparing our firmware with test B (phase 47e): the whole system block
 * 0x40001000-0x400010FF (clocks, sleep and power control, pins, RTC, ADC, GPIO), then a few peripheral
 * registers and the PFIC enable and pending bits. Only reads; needs CH58x_common.h.
 *
 * Text: "regs " then 64 words of the system block as 8 hex digits (word n = address 0x40001000 + 4n),
 * then the extras in the order of regdump_extra[] below. Only 0-9, a-f, space, so both programs can type
 * it with a minimal key map.
 */
#pragma once

#include "CH58x_common.h"

#define REGDUMP_SYS_WORDS 64
#define REGDUMP_EXTRA_N   14
#define REGDUMP_TEXT_LEN  (5 + (REGDUMP_SYS_WORDS + REGDUMP_EXTRA_N) * 9 + 1)

typedef struct {
    uint32_t sys[REGDUMP_SYS_WORDS];
    uint32_t extra[REGDUMP_EXTRA_N];
} regdump_t;

static inline void regdump_take(regdump_t *d) {
    for (uint8_t i = 0; i < REGDUMP_SYS_WORDS; i++) {
        d->sys[i] = *(volatile uint32_t *)(0x40001000 + 4 * i);
    }
    uint8_t n = 0;
    d->extra[n++] = R8_TMR0_CTRL_MOD | (R8_TMR1_CTRL_MOD << 8) | (R8_TMR2_CTRL_MOD << 16) | (R8_TMR3_CTRL_MOD << 24);
    d->extra[n++] = R8_TMR1_CTRL_DMA;
    d->extra[n++] = R8_USB2_CTRL | (R8_USBHS_PLL_CTRL << 8);
    d->extra[n++] = R8_USB_CTRL | (R8_UDEV_CTRL << 8);
    d->extra[n++] = R8_UART1_IER | (R8_UART1_LCR << 8);
    d->extra[n++] = R8_SPI0_CTRL_MOD;
    d->extra[n++] = R16_I2C_CTRL1;
    d->extra[n++] = PFIC->ISR[0];
    d->extra[n++] = PFIC->ISR[1];
    d->extra[n++] = PFIC->IPR[0];
    d->extra[n++] = PFIC->IPR[1];
    d->extra[n++] = PFIC->ITHRESDR;
    d->extra[n++] = PFIC->SCTLR;
    uint32_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    d->extra[n++] = sp;
}

/* Returns the text length (without the terminating 0) */
static inline uint16_t regdump_text(const regdump_t *d, char *out) {
    static const char hex[] = "0123456789abcdef";
    char *p = out;
    *p++ = 'r'; *p++ = 'e'; *p++ = 'g'; *p++ = 's'; *p++ = ' ';
    for (uint8_t i = 0; i < REGDUMP_SYS_WORDS + REGDUMP_EXTRA_N; i++) {
        uint32_t v = i < REGDUMP_SYS_WORDS ? d->sys[i] : d->extra[i - REGDUMP_SYS_WORDS];
        for (int8_t s = 28; s >= 0; s -= 4) {
            *p++ = hex[(v >> s) & 0xF];
        }
        *p++ = ' ';
    }
    *p = 0;
    return p - out;
}
