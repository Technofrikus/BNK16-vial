/*
 * BNK16 UF2 bootloader
 *
 * Sits at 0x0000, the firmware at APP_START. On every reset it decides:
 *
 *   - firmware asked for it (R8_GLOB_RESET_KEEP == 5, set by QK_BOOT / bootloader_jump()) -> bootloader
 *   - no valid firmware (first word erased)                                              -> bootloader
 *   - power-on reset while the top-left key is held                                      -> bootloader
 *   - otherwise                                                                          -> jump to the firmware
 *
 * The bootloader shows up as a USB drive "BNK16BOOT". Copying a .uf2 onto it writes the firmware
 * and reboots into it. The ROM ISP bootloader (BOOT key + wchisp) stays untouched as the last resort.
 */

#include "CH58x_common.h"
#include "bootloader.h"

/* Top-left key: matrix row 0 = PB3 (output), col 0 = PB5 (input), diodes COL2ROW */
#define KEY_ROW_PIN (1u << 3)
#define KEY_COL_PIN (1u << 5)

static bool app_present(void)
{
    uint32_t first = *(volatile const uint32_t *)APP_START;
    return first != 0xFFFFFFFFu && first != 0x00000000u;
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

__attribute__((noreturn)) static void jump_to_app(void)
{
    /* Nothing was set up besides the two GPIOs (restored above), no interrupt is enabled. The firmware's
     * own startup sets gp, sp, the clock, the RAM vector table and mtvec again. */
    __asm__ volatile("csrci mstatus, 0x8");
    ((void (*)(void))APP_START)();
    __builtin_unreachable();
}

int main(void)
{
    bool enter = false;

    if (R8_GLOB_RESET_KEEP == RESET_KEEP_ENTER_BOOTLOADER) {
        enter = true;
    }
    R8_GLOB_RESET_KEEP = 0;

    if (!app_present()) {
        enter = true;
    }
    if (!enter && reset_was_power_on()) {
        enter = boot_key_held();
    }
    if (!enter) {
        jump_to_app();
    }

    /* USB high-speed needs the HSE PLL clock, same as the firmware */
    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(CLK_SOURCE_HSE_PLL_62_4MHz);

    usb_msc_init();

    uint32_t settle_ms = 0;
    for (;;) {
        usb_msc_poll();

        if (uf2_update_done() && usb_msc_idle()) {
            /* let the host finish its directory and FAT writes, then reboot into the new firmware */
            DelayMs(1);
            if (++settle_ms >= 500) {
                DelayMs(10);
                SYS_ResetExecute();
            }
        }
    }
}
