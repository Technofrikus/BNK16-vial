/*
 * BNK16 UF2 bootloader
 *
 * Sits at 0x0000, the firmware at APP_START. On every reset it decides:
 *
 *   - firmware asked for it (R8_GLOB_RESET_KEEP == 5, set by QK_BOOT / bootloader_jump()) -> bootloader
 *   - no valid firmware (first word erased, or an update through us that never completed) -> bootloader
 *   - power-on reset while the top-left key is held                                      -> bootloader
 *   - otherwise                                     -> jump to our firmware, or to a Binepad stock image
 *
 * The stock firmware uses the same reset-keep value for its own "update mode" key, so from stock the
 * update key also lands here.
 *
 * The bootloader shows up as a USB drive "BNK16BOOT". Copying a .uf2 onto it writes the firmware
 * and reboots into it. The ROM ISP bootloader (BOOT key + wchisp) stays untouched as the last resort.
 */

#include "CH58x_common.h"
#include "bootloader.h"

/* Top-left key: matrix row 0 = PB3 (output), col 0 = PB5 (input), diodes COL2ROW */
#define KEY_ROW_PIN (1u << 3)
#define KEY_COL_PIN (1u << 5)

/* Idle timeout: one main-loop pass takes about IDLE_TICK_US plus the polling itself, so this is roughly
 * ten minutes without a USB transfer or bus reset. Not a precise clock. */
#define IDLE_TICK_US   20u
#define IDLE_TICKS_MAX (10u * 60u * 1000000u / (IDLE_TICK_US + 2u))

static bool word_programmed(uint32_t addr)
{
    uint32_t w = *(volatile const uint32_t *)addr;
    return w != 0xFFFFFFFFu && w != 0x00000000u;
}

static uint32_t stock_entry(void)
{
    if (*(volatile const uint32_t *)STOCK_SLOT == STOCK_IMAGE_MAGIC) {
        uint32_t hdr_size = *(volatile const uint16_t *)(STOCK_SLOT + 8);
        uint32_t entry = STOCK_SLOT + hdr_size;
        if (hdr_size >= 32 && hdr_size <= 0x1000 && (hdr_size % 4) == 0 && word_programmed(entry)) {
            return entry;
        }
    }
    return 0;
}

/* Where to start the firmware, 0 if there is none. After an update through this bootloader the state
 * block says which image is complete (and an interrupted update means none). Without it (board flashed
 * with wchisp) ours wins and a Binepad stock image is only started when ours is absent. */
static uint32_t firmware_entry(void)
{
    uint32_t state = image_state();

    if (state == IMAGE_STATE_INVALID) {
        return 0;
    }
    if (state == APP_START) {
        return word_programmed(APP_START) ? APP_START : 0;
    }
    if (state == STOCK_SLOT) {
        return stock_entry();
    }
    if (word_programmed(APP_START)) {
        return APP_START;
    }
    return stock_entry();
}

static bool boot_key_held(void)
{
    /* column: input with pull-up; row: driven low. A pressed key pulls the column low. */
    R32_PB_DIR &= ~KEY_COL_PIN;
    R32_PB_PU |= KEY_COL_PIN;
    R32_PB_PD_DRV &= ~KEY_COL_PIN;
    R32_PB_CLR = KEY_ROW_PIN;
    R32_PB_DIR |= KEY_ROW_PIN;

    DelayMs(5);
    bool held = true;
    for (int i = 0; i < 10 && held; i++) {
        if (R32_PB_PIN & KEY_COL_PIN) {
            held = false;
        }
        DelayMs(2);
    }

    /* back to reset state (floating inputs) so the firmware starts from a clean slate */
    R32_PB_DIR &= ~(KEY_ROW_PIN | KEY_COL_PIN);
    R32_PB_PU &= ~(KEY_ROW_PIN | KEY_COL_PIN);
    return held;
}

static bool reset_was_power_on(void)
{
    switch (R8_RESET_STATUS & RB_RESET_FLAG) {
        case RST_FLAG_SW:    /* firmware reboot, also the wake-up path from the idle shutdown */
        case RST_FLAG_WTR:   /* watchdog */
        case RST_FLAG_GPWSM: /* woken from shutdown mode */
            return false;
        default:             /* real power-on, reset pin, power-on during sleep */
            return true;
    }
}

__attribute__((noreturn)) static void jump_to(uint32_t entry)
{
    /* Nothing was set up besides the two GPIOs (restored above), no interrupt is enabled. The firmware's
     * own startup sets gp, sp, the clock, the RAM vector table and mtvec again (checked for both ours
     * and the stock 1.7.141 image). */
    __asm__ volatile("csrci mstatus, 0x8");
    ((void (*)(void))entry)();
    __builtin_unreachable();
}

int main(void)
{
    bool enter = false;

    if (R8_GLOB_RESET_KEEP == RESET_KEEP_ENTER_BOOTLOADER) {
        enter = true;
    }
    R8_GLOB_RESET_KEEP = 0;

    uint32_t entry = firmware_entry();
    if (entry == 0) {
        enter = true;
    }
    if (!enter && reset_was_power_on()) {
        enter = boot_key_held();
    }
    if (!enter) {
        jump_to(entry);
    }

    /* USB high-speed needs the HSE PLL clock, same as the firmware */
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(CLK_SOURCE_HSE_PLL_62_4MHz);

    usb_msc_init();

    uint32_t settle_ms = 0;
    uint32_t idle_ticks = 0;
    for (;;) {
        if (usb_msc_poll()) {
            idle_ticks = 0;
        }

        if (uf2_update_done() && usb_msc_idle()) {
            /* let the host finish its directory and FAT writes, then reboot into the new firmware */
            DelayMs(1);
            if (++settle_ms >= 500) {
                DelayMs(10);
                SYS_ResetExecute();
            }
        } else {
            settle_ms = 0;                     /* count 500 ms in a row without host traffic */
        }

        /* The keyboard runs on a battery too: with nobody talking to us (cable pulled, host asleep, the
         * mode entered by accident) go back to the firmware instead of polling until the battery is empty.
         * Without a firmware there is nothing to go back to, so keep waiting. */
        DelayUs(IDLE_TICK_US);
        if (++idle_ticks >= IDLE_TICKS_MAX) {
            idle_ticks = 0;
            if (firmware_entry() != 0) {
                SYS_ResetExecute();
            }
        }
    }
}
