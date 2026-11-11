/*
 * BNK16 deep sleep diagnostics (phases 47 and 48), test builds only. Nothing here is compiled into the
 * normal firmware: bnk16_sleep_diag.h turns every hook into an empty inline function unless one of the
 * switches below is given with BNK16_DEFS (build-bnk16.sh, rules.cmake). How to read the typed lines:
 * docs/bnk16/PHASE47_HANDOVER.md.
 *
 *   BNK16_FAULT_DIAG  crash and hang reports: own HardFault/NMI handlers that keep the fault registers in a
 *                     RAM block over a software reset (bnk16_fault.h), a hardware and a TMR3 soft watchdog
 *                     armed only in deep sleep, and the "fault", "stats", "wake" and "lsi" lines typed over
 *                     BLE. The first deep sleep after boot ends by itself after DS_TIMED_EXIT_MS.
 *                     Counters for it are also in ble/ble_app.c, protocol/protocol_usb.c, sdk/HAL/SLEEP.c,
 *                     sdk/HAL/RTC.c and user/usb/ch585_usbhs_device.c.
 *   BNK16_BOOT_DIAG   reset cause as the status LED colour for 4 s after boot
 *   BNK16_REGDUMP     register snapshot at the first deep sleep entry, typed after the wake-up
 *
 * Do not use these builds with Vial writes (watchdog in deep sleep).
 */
#include "bnk16_sleep_diag.h"

#ifdef BNK16_SLEEP_DIAG
#include "ble_app.h"
#include "power_management.h"
#include "CH58x_common.h"
#ifdef BNK16_REGDUMP
#include "../../../tests/common/bnk16_regdump.h" /* phase 47e, see regdump_task() */
#endif
#include <string.h>
#include "bnk16_fault.h"

#ifdef BNK16_FAULT_DIAG
/* Test build (phase 47g), see bnk16_fault.h. The fault handler replaces the SDK's weak one. */
__INTERRUPT __HIGH_CODE void HardFault_Handler(void) {
    uint32_t mcause, mepc, mtval, sp;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    __asm__ volatile("csrr %0, mtval" : "=r"(mtval));
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    BNK16_FAULT->magic  = FAULT_DIAG_MAGIC;
    BNK16_FAULT->fault  = 1;
    BNK16_FAULT->mcause = mcause;
    BNK16_FAULT->mepc   = mepc;
    BNK16_FAULT->mtval  = mtval;
    BNK16_FAULT->sp     = sp;
    FLASH_ROM_SW_RESET();
    sys_safe_access_enable();
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET; /* plain software reset: RAM stays */
    sys_safe_access_disable();
    while (1) {
    }
}

__INTERRUPT __HIGH_CODE void NMI_Handler(void) {
    uint32_t mepc;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    BNK16_FAULT->magic = FAULT_DIAG_MAGIC;
    BNK16_FAULT->fault = 2;
    BNK16_FAULT->mepc  = mepc;
    sys_safe_access_enable();
    R8_RST_WDOG_CTRL |= RB_SOFTWARE_RESET;
    sys_safe_access_disable();
    while (1) {
    }
}

static bnk16_fault_t fault_report;  /* the block as found at boot */
static bool          fault_report_valid, fault_report_typed;

/* At the start of main(): keep what the last run left, then start a fresh block */
static void fault_diag_boot(void) {
    fault_report_valid = BNK16_FAULT->magic == FAULT_DIAG_MAGIC;
    if (fault_report_valid) {
        fault_report = *(bnk16_fault_t *)BNK16_FAULT;
    }
    BNK16_FAULT->magic  = FAULT_DIAG_MAGIC;
    BNK16_FAULT->fault  = 0;
    BNK16_FAULT->stage  = 0;
    BNK16_FAULT->sleeps = 0;
    BNK16_FAULT->wakes  = 0;
    BNK16_FAULT->calls  = 0;
    BNK16_FAULT->shorts = 0;
    BNK16_FAULT->lates  = 0;
    BNK16_FAULT->pins   = 0;
    BNK16_FAULT->loops  = 0;
    BNK16_FAULT->skips  = 0;
    BNK16_FAULT->isr_rtc = BNK16_FAULT->isr_gpio = BNK16_FAULT->isr_bb = 0;
    BNK16_FAULT->isr_usb = BNK16_FAULT->usb_flag = 0;
}

/* Watchdog (phase 47i/k): a hang resets the chip after about 0.5 s (counter at Fsys/131072 = 476 Hz, 256
 * counts) and keeps the RAM block, so the next boot reports the stage it hung in (reset flag 2, cyan).
 * It stops while the chip sleeps. Armed only from the deep sleep entry until 2 s after its exit (47j had
 * it on all the time: its enable survives a software reset, so the UF2 bootloader inherited it and could
 * reset in the middle of a firmware update). Fed from housekeeping, deep sleep passes, the shutdown. */
static bool     wdog_armed;
static uint32_t wdog_disarm_at;

__HIGH_CODE void bnk16_wdog_feed(void) {
    WWDG_SetCounter(0);
    BNK16_FAULT->fed++;
}

/* Soft watchdog (phase 47l): TMR3 interrupt every 10 ms while armed; if the feed counter has not moved
 * for SOFT_WDOG_TICKS, it records where the CPU was (mepc of the interrupted code) and the interrupt
 * state, then resets with the RAM kept (fault 3). If an interrupt storm or masked interrupts keep it
 * from running, the hardware watchdog resets instead (fault 0, r2). */
#define SOFT_WDOG_TICKS 30
__INTERRUPT __HIGH_CODE void TMR3_IRQHandler(void) {
    static uint32_t last_fed, idle_ticks;
    TMR3_ClearITFlag(TMR0_3_IT_CYC_END);
    if (BNK16_FAULT->fed != last_fed) {
        last_fed   = BNK16_FAULT->fed;
        idle_ticks = 0;
        return;
    }
    if (++idle_ticks < SOFT_WDOG_TICKS) {
        return;
    }
    uint32_t mepc, mcause, sp;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    BNK16_FAULT->magic  = FAULT_DIAG_MAGIC;
    BNK16_FAULT->fault  = 3;
    BNK16_FAULT->mepc   = mepc;
    BNK16_FAULT->mcause = mcause;
    BNK16_FAULT->mtval  = 0;
    BNK16_FAULT->sp     = sp;
    BNK16_FAULT->gisr   = PFIC->GISR;
    BNK16_FAULT->ipr0   = PFIC->IPR[0];
    BNK16_FAULT->ipr1   = PFIC->IPR[1];
    BNK16_FAULT->ien0   = PFIC->ISR[0];
    BNK16_FAULT->ien1   = PFIC->ISR[1];
    sys_safe_access_enable();
    R8_RST_WDOG_CTRL = RB_SOFTWARE_RESET; /* watchdog reset off, software reset now */
    sys_safe_access_disable();
    while (1) {
    }
}

static void wdog_arm(bool on) {
    WWDG_SetCounter(0);
    WWDG_ResetCfg(on ? ENABLE : DISABLE);
    if (on) {
        TMR3_TimerInit(FREQ_SYS / 100);
        TMR3_ClearITFlag(TMR0_3_IT_CYC_END);
        TMR3_ITCfg(ENABLE, TMR0_3_IT_CYC_END);
        PFIC_EnableIRQ(TMR3_IRQn);
    } else {
        PFIC_DisableIRQ(TMR3_IRQn);
        TMR3_ITCfg(DISABLE, TMR0_3_IT_CYC_END);
        R8_TMR3_CTRL_MOD = RB_TMR_ALL_CLEAR;
    }
    wdog_armed     = on;
    wdog_disarm_at = 0;
}

static void wdog_task(void) {
    bnk16_wdog_feed();
    if (wdog_armed && wdog_disarm_at && !power_in_deep_sleep() && timer_read32() - wdog_disarm_at < 0x80000000u) {
        wdog_arm(false);
    }
}

static void hex_word(char *p, uint32_t v);

/* send_string() blocks for about 20 ms per character, longer than the watchdog allows for a whole line
 * (47i reset itself in the middle of every report): type one character at a time and feed it */
static void type_line(const char *line) {
    for (const char *c = line; *c; c++) {
        char one[2] = { *c, 0 };
        send_string(one);
        bnk16_wdog_feed();
    }
}

/* Sleep statistics: the first deep sleep after boot ends by itself after DS_TIMED_EXIT_MS (no pin, so the
 * key wake-up path is left out), then this line is typed at once (the deep sleep starts again 100 ms
 * later, and send_string() blocks until then); later deep sleeps end only by a key */
#define DS_TIMED_EXIT_MS 20000
static uint32_t stats_exit_time;
static bool     stats_typed;

uint32_t bnk16_loop_gap_max;

/* Phase 47x: the 32 kHz sleep clock (internal RC, calibrated to 32000 Hz by the SDK) measured against
 * the crystal: SysTick counts the 62.4 MHz system clock over 3200 RTC ticks (100 ms). Types
 * "lsi fF tT": F in mHz (32000000 = 1e84800 is exact; 1000 ppm = 32000 = 0x7d00 off), T the chip
 * temperature sensor reading the BLE stack uses. The BLE stack is told the clock is good to 1000 ppm. */
extern uint16_t HAL_GetInterTempValue(void);

static void type_lsi(void) {
    uint32_t t0, t1, c0, c1;
    bnk16_wdog_feed();
    /* the chip sleep stops SysTick (47x typed ffffffff after the first deep sleep); the BLE stack only
     * takes random seeds from it */
    if (!(SysTick->CTLR & SysTick_CTLR_STE)) {
        SysTick->CMP  = SysTick_LOAD_RELOAD_Msk - 1;
        SysTick->CTLR = SysTick_CTLR_STRE | SysTick_CTLR_STCLK | SysTick_CTLR_STE;
    }
    t0 = R32_RTC_CNT_32K;
    do {
        t1 = R32_RTC_CNT_32K;
    } while (t1 == t0);
    c0 = SysTick->CNTL;
    do {
        t0 = R32_RTC_CNT_32K;
    } while (t0 - t1 < 3200 || t0 != R32_RTC_CNT_32K);
    c1 = SysTick->CNTL;
    bnk16_wdog_feed();
    uint32_t ticks = t0 - t1;
    uint32_t mhz   = (uint32_t)(((uint64_t)ticks * FREQ_SYS * 1000ULL) / (uint32_t)(c1 - c0));
    char     line[56];
    char    *p = line;
    memcpy(p, "lsi f", 5);
    p += 5;
    hex_word(p, mhz);
    p += 8;
    *p++ = ' ';
    *p++ = 't';
    hex_word(p, (uint32_t)HAL_GetInterTempValue());
    p += 8;
    *p++ = ' ';
    *p++ = 'w'; /* phase 48: longest wait for the BLE stack to take a keyboard report since boot, ms */
    hex_word(p, ble_send_wait_max);
    p += 8;
    *p++ = '\n';
    *p   = 0;
    type_line(line);
}

static void stats_task(void) {
    if (stats_typed || !stats_exit_time || !ble_is_connected()) {
        return;
    }
    stats_typed = true;
    const struct {
        char     tag;
        uint32_t value;
    } f[] = {
        { 'c', BNK16_FAULT->calls },  { 'n', BNK16_FAULT->sleeps }, { 'w', BNK16_FAULT->wakes },
        { 'k', BNK16_FAULT->shorts }, { 'l', BNK16_FAULT->lates },  { 'p', BNK16_FAULT->pins },
        { 'm', BNK16_FAULT->loops },  { 'h', BNK16_FAULT->skips },  { 's', BNK16_FAULT->stage },
        /* phase 47w: links started and ended since boot, longest loop gap (RTC ticks) */
        { 'e', ble_link_diag.connects }, { 'x', ble_link_diag.losses }, { 'b', bnk16_loop_gap_max },
    };
    char  line[160];
    char *p = line;
    memcpy(p, "stats", 5);
    p += 5;
    for (uint8_t i = 0; i < ARRAY_SIZE(f); i++) {
        *p++ = ' ';
        *p++ = f[i].tag;
        hex_word(p, f[i].value);
        p += 8;
    }
    *p++ = '\n';
    *p   = 0;
    type_line(line);
    type_line(line);
    type_lsi();
}

static void hex_word(char *p, uint32_t v) {
    for (int8_t s = 28; s >= 0; s -= 4) {
        *p++ = "0123456789abcdef"[(v >> s) & 0xF];
    }
}

/* Types the report twice, 5 s after a host is there (BLE in the BT position, USB in the USB one):
 *   "nodata rN"                 RAM did not keep the block: a real power loss (or the first boot)
 *   "fault F cC eE tT pP sS nN wW rR"  F: 0 = no fault (a reset without a crash), 1 = exception, 2 = NMI;
 *       C mcause, E mepc (address of the fault), T mtval, P stack pointer, S last stage, N sleeps,
 *       W wake-ups, R reset flag of this boot */
static void fault_diag_task(void) {
    /* 5 s after the host is there: the first characters after a reconnect used to get lost */
    /* separate flag: "timer | 1" as a set marker lay 1 ms in the future on even times, and
     * timer_elapsed32() of a future time is huge (47o: the waits ended at once) */
    static uint32_t host_since;
    static bool     host_seen;
    if (fault_report_typed) {
        return;
    }
    if (wired_mode_active ? !bnk16_usb_is_connected() : !ble_is_connected()) {
        host_seen = false;
        return;
    }
    if (!host_seen) {
        host_seen  = true;
        host_since = timer_read32();
    }
    if (timer_elapsed32(host_since) < 5000) {
        return;
    }
    fault_report_typed = true;
    char    line[288];
    char   *p    = line;
    uint8_t flag = R8_RESET_STATUS & RB_RESET_FLAG;
    if (!fault_report_valid) {
        memcpy(p, "nodata r", 8);
        p += 8;
        *p++ = '0' + flag;
    } else {
        const struct {
            char     tag;
            uint32_t value;
        } f[] = {
            { 'c', fault_report.mcause }, { 'e', fault_report.mepc },  { 't', fault_report.mtval },
            { 'p', fault_report.sp },     { 's', fault_report.stage }, { 'n', fault_report.sleeps },
            { 'w', fault_report.wakes },  { 'x', fault_report.pins },  { 'h', fault_report.skips },
            { 'q', fault_report.isr_rtc }, { 'o', fault_report.isr_gpio }, { 'b', fault_report.isr_bb },
            { 'g', fault_report.gisr },   { 'i', fault_report.ipr0 },  { 'j', fault_report.ipr1 },
            { 'u', fault_report.ien0 },   { 'v', fault_report.ien1 },
            { 'y', fault_report.isr_usb }, { 'z', fault_report.usb_flag },
        };
        memcpy(p, "fault ", 6);
        p += 6;
        *p++ = '0' + (fault_report.fault & 7);
        for (uint8_t i = 0; i < ARRAY_SIZE(f); i++) {
            *p++ = ' ';
            *p++ = f[i].tag;
            hex_word(p, f[i].value);
            p += 8;
        }
        *p++ = ' ';
        *p++ = 'r';
        *p++ = '0' + flag;
    }
    *p++ = '\n';
    *p   = 0;
    type_line(line);
    type_line(line); /* twice, in case the start of the first one got lost */
    type_lsi();
}
/* Wake-up report (phase 47r): is the waking key seen by QMK? One record per deep sleep exit, kept until
 * the board has been awake for 3 s (47q typed 3 s after each exit and never got there: without a key
 * event the deep sleep starts again after POWER_DEEP_SLEEP_RETRY_MS). On the third exit without a key
 * event the board is kept awake so the records get typed anyway. Hex without leading zeros:
 *   "wake nN iI cC tT sS mM kK pP dD aA"  N: number of the exit, I: 1 = the pin interrupt fired,
 *   C: column pins PB4-PB7 at the exit (0 bit = pressed), T: RTC ticks (31 us) from the pin interrupt to
 *   the exit, S: QMK scans in the awake time (until the first key event), M: all matrix bits seen in
 *   them (5 bits per row), K: keycode of the first event (ffff = none), P: 1 press / 0 release, D: ms
 *   from the exit to it, A: ms awake before the next deep sleep (0 = stayed awake), then L H Y R W O:
 *   the BLE side, see wake_diag_send() */
typedef struct {
    uint32_t n, irq, cols, ticks, scans, mat, kc, pressed, dt, awake;
    uint32_t h, y, r, w, o, lvl; /* phase 47s, see wake_diag_send() */
    uint32_t g, e, x, q, j, u;   /* phase 47t, the link at the exit */
} wake_rec_t;
#define WAKE_RECS 4
static volatile uint32_t wake_irq_rtc;
static volatile bool     wake_irq_seen;
static wake_rec_t        wake_recs[WAKE_RECS];
static uint8_t           wake_pending;
static uint32_t          wake_exit_time, wake_total;
static bool              wake_open, wake_seen, wake_timed;

/* The first keyboard report after the exit, from ble_app.c: H result of its first try (0 = taken, ee =
 * no link, ff = no report), Y its first key byte, R tries, W ms until the stack took it (ffff = it did
 * not), O 1 = a later report replaced it first */
static void wake_diag_send(wake_rec_t *r) {
    r->h = ble_send_diag.tries ? ble_send_diag.first_status : 0xFF;
    r->y = ble_send_diag.key;
    r->r = ble_send_diag.tries;
    r->w = ble_send_diag.sent ? ble_send_diag.sent_ms : 0xFFFF;
    r->o = ble_send_diag.overwritten;
}

static void wake_diag_enter(void) {
    if (wake_open) {
        wake_diag_send(&wake_recs[wake_pending - 1]);
        wake_recs[wake_pending - 1].awake = timer_elapsed32(wake_exit_time);
        wake_open                         = false;
    }
    wake_irq_seen = false;
}

static void wake_diag_exit(void) {
    if (wake_timed) { /* the 20 s timed exit of the first deep sleep: no key */
        wake_timed = false;
        return;
    }
    if (wake_pending == WAKE_RECS) {
        memmove(&wake_recs[0], &wake_recs[1], sizeof(wake_recs) - sizeof(wake_recs[0]));
        wake_pending--;
    }
    wake_rec_t *r = &wake_recs[wake_pending++];
    memset(r, 0, sizeof(*r));
    r->n           = ++wake_total;
    r->irq         = wake_irq_seen;
    r->cols        = (R32_PB_PIN >> 4) & 0x0F;
    r->ticks       = R32_RTC_CNT_32K - wake_irq_rtc;
    r->kc          = 0xFFFF;
    r->lvl         = ble_get_current_interval_level(); /* L: BLE interval level at the exit, 1 = slow */
    /* G: BLE state at the exit (5 = connected, 2 = reconnecting), E: links started since boot, X: links
     * ended, Q: reason of the last end (8 = timeout, 13 = host, 16 = board), J: ms since the last link
     * start, U: ms since the last end */
    r->g = ble_get_state();
    r->e = ble_link_diag.connects;
    r->x = ble_link_diag.losses;
    r->q = ble_link_diag.reason;
    r->j = timer_elapsed32(ble_link_diag.connect_time);
    r->u = ble_link_diag.losses ? timer_elapsed32(ble_link_diag.loss_time) : 0;
    memset(&ble_send_diag, 0, sizeof(ble_send_diag));
    ble_send_diag.armed = 1;
    wake_exit_time = timer_read32();
    wake_seen      = false;
    wake_open      = true;
    if (wake_pending >= 3) {
        power_notify_activity(); /* stay awake: the records are typed 3 s from now */
    }
}

static void wake_diag_event(uint16_t keycode, bool pressed) {
    if (wake_open && !wake_seen) {
        wake_rec_t *r = &wake_recs[wake_pending - 1];
        wake_seen     = true;
        r->kc         = keycode;
        r->pressed    = pressed;
        r->dt         = timer_elapsed32(wake_exit_time);
    }
}

static void wake_diag_scan(void) {
    if (wake_open && !wake_seen) {
        wake_rec_t *r = &wake_recs[wake_pending - 1];
        r->scans++;
        for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
            r->mat |= (uint32_t)matrix_get_row(row) << (5 * row);
        }
    }
}

static void wake_diag_task(void) {
    if (!wake_pending || timer_elapsed32(wake_exit_time) < 3000 || !ble_is_connected()) {
        return;
    }
    uint8_t count = wake_pending;
    if (wake_open) {
        wake_diag_send(&wake_recs[count - 1]);
    }
    ble_send_diag.armed = 0;
    wake_pending        = 0;
    wake_open           = false;
    for (uint8_t i = 0; i < count; i++) {
        const wake_rec_t *r = &wake_recs[i];
        const struct {
            char     tag;
            uint32_t value;
        } f[] = {
            { 'n', r->n },     { 'i', r->irq }, { 'c', r->cols },    { 't', r->ticks }, { 's', r->scans },
            { 'm', r->mat },   { 'k', r->kc },  { 'p', r->pressed }, { 'd', r->dt },    { 'a', r->awake },
            { 'l', r->lvl },   { 'h', r->h },   { 'y', r->y },       { 'r', r->r },     { 'w', r->w },
            { 'o', r->o },     { 'g', r->g },   { 'e', r->e },       { 'x', r->x },     { 'q', r->q },
            { 'j', r->j },     { 'u', r->u },   { 'f', ble_link_diag.first_reason },
            { 'v', ble_link_diag.first_time }, /* F, V: reason and time since boot (ms) of the first end */
            { 'b', bnk16_loop_gap_max },       /* B: longest gap between TMOS runs in deep sleep, RTC ticks */
        };
        char  line[224];
        char *p = line;
        memcpy(p, "wake", 4);
        p += 4;
        for (uint8_t j = 0; j < ARRAY_SIZE(f); j++) {
            char word[8];
            hex_word(word, f[j].value);
            uint8_t first = 0;
            while (first < 7 && word[first] == '0') {
                first++;
            }
            *p++ = ' ';
            *p++ = f[j].tag;
            memcpy(p, word + first, 8 - first);
            p += 8 - first;
        }
        *p++ = '\n';
        *p   = 0;
        if (i == 0) {
            type_line("................"); /* the first characters typed get lost (47r: up to 12) */
        }
        type_line(line);
    }
    type_lsi();
}

/* process_record_kb belongs to the port (protocol/extra_keycode.c); the keymap has no pre_process hook */
bool pre_process_record_user(uint16_t keycode, keyrecord_t *record) {
    wake_diag_event(keycode, record->event.pressed);
    return true;
}
#endif

#ifdef BNK16_REGDUMP
/* Test build (phase 47e): register snapshot at the first deep sleep entry, typed 2 s after the next
 * wake-up from it (compare with test B, tests/common/bnk16_regdump.h) */
static regdump_t regdump;
static bool      regdump_taken, regdump_typed;
static uint32_t  regdump_wake_time;

static void regdump_task(void) {
    if (!regdump_taken || regdump_typed || !regdump_wake_time ||
        timer_elapsed32(regdump_wake_time) < 2000 || !ble_is_connected()) {
        return;
    }
    static char text[REGDUMP_TEXT_LEN];
    regdump_typed = true;
    regdump_text(&regdump, text);
    send_string(text);
    send_string("\n");
}
#endif

#ifdef BNK16_BOOT_DIAG
/* Why the chip last reset, read at the start of main() (the bootloader does not change it).
 * Shown for BOOT_DIAG_MS after boot, before anything else; then 1 s off. */
#define BOOT_DIAG_MS 4000
static uint8_t boot_reset_flag;

static bool boot_diag_color(rgb_t *color) {
    uint32_t t = timer_read32();
    if (t >= BOOT_DIAG_MS + 1000) {
        return false;
    }
    if (t >= BOOT_DIAG_MS) {
        *color = (rgb_t){.r = 0, .g = 0, .b = 0};
        return true;
    }
    switch (boot_reset_flag) {
        case RST_FLAG_RPOR: *color = (rgb_t){.r = 255, .g = 255, .b = 255}; break; /* white: real power on */
        case RST_FLAG_SW:   *color = (rgb_t){.r = 255, .g = 160, .b = 0}; break;   /* yellow: software reset */
        case RST_FLAG_WTR:  *color = (rgb_t){.r = 0, .g = 255, .b = 255}; break;   /* cyan: watchdog */
        case RST_FLAG_MR:   *color = (rgb_t){.r = 255, .g = 0, .b = 255}; break;   /* magenta: reset pin */
        case 5:             *color = (rgb_t){.r = 0, .g = 255, .b = 0}; break;     /* green: woken from shutdown */
        default:            *color = (rgb_t){.r = 255, .g = 0, .b = 0}; break;     /* red: power-on reset during sleep (4, 6, 7) */
    }
    return true;
}
#endif

/* ========================================================================
 * Hooks called from bnk16.c, see bnk16_sleep_diag.h
 * ======================================================================== */

void bnk16_sleep_diag_boot(void) {
#ifdef BNK16_BOOT_DIAG
    boot_reset_flag = R8_RESET_STATUS & RB_RESET_FLAG;
#endif
#ifdef BNK16_FAULT_DIAG
    WWDG_ResetCfg(DISABLE); /* in case a software reset kept it on */
    fault_diag_boot();
#endif
}

void bnk16_sleep_diag_task(void) {
#ifdef BNK16_FAULT_DIAG
    wake_diag_task();
    wdog_task();
    fault_diag_task();
    stats_task();
#endif
#ifdef BNK16_REGDUMP
    regdump_task();
#endif
}

void bnk16_sleep_diag_scan(void) {
#ifdef BNK16_FAULT_DIAG
    wake_diag_scan();
#endif
}

bool bnk16_sleep_diag_boot_color(rgb_t *color) {
#ifdef BNK16_BOOT_DIAG
    return boot_diag_color(color);
#else
    return false;
#endif
}

__HIGH_CODE void bnk16_sleep_diag_feed(void) {
#ifdef BNK16_FAULT_DIAG
    bnk16_wdog_feed();
#endif
}

__HIGH_CODE void bnk16_sleep_diag_gpio_irq(void) {
#ifdef BNK16_FAULT_DIAG
    BNK16_COUNT(isr_gpio);
    wake_irq_rtc  = R32_RTC_CNT_32K;
    wake_irq_seen = true;
#endif
}

void bnk16_sleep_diag_ds_enter(void) {
#ifdef BNK16_FAULT_DIAG
    wdog_arm(true);
#endif
}

void bnk16_sleep_diag_ds_enter_irq_off(void) {
#ifdef BNK16_FAULT_DIAG
    wake_diag_enter();
#endif
}

void bnk16_sleep_diag_ds_armed(void) {
#ifdef BNK16_REGDUMP
    if (!regdump_taken) {
        regdump_taken = true;
        regdump_take(&regdump);
    }
#endif
}

bool bnk16_sleep_diag_ds_poll(void) {
#ifdef BNK16_FAULT_DIAG
    static bool     timed_exit_done, ds_started;
    static uint32_t ds_start;
    BNK16_COUNT(loops);
    bnk16_wdog_feed();
    if (!ds_started) {
        ds_started = true;
        ds_start   = timer_read32();
    }
    if (!timed_exit_done && timer_elapsed32(ds_start) >= DS_TIMED_EXIT_MS) {
        timed_exit_done = true;
        stats_exit_time = timer_read32() | 1;
        wake_timed      = true;
        return true;
    }
#endif
    return false;
}

void bnk16_sleep_diag_ds_exit(void) {
#ifdef BNK16_FAULT_DIAG
    wake_diag_exit(); /* before the rows go back to QMK: a held key still pulls its column low */
#endif
}

void bnk16_sleep_diag_ds_exit_done(void) {
#ifdef BNK16_FAULT_DIAG
    wdog_disarm_at = (timer_read32() + 2000) | 1;
#endif
#ifdef BNK16_REGDUMP
    if (regdump_taken && !regdump_wake_time) {
        regdump_wake_time = timer_read32() | 1; /* only a set marker; the 2 s wait is approximate */
    }
#endif
}

void bnk16_sleep_diag_shutdown(void) {
#ifdef BNK16_FAULT_DIAG
    WWDG_ResetCfg(DISABLE);
#endif
}

#endif /* BNK16_SLEEP_DIAG */
