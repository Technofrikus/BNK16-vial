/*
 * BNK16 test B: WCH's official BLE HID keyboard example (EVT/EXAM/BLE/HID_Keyboard, openwch/ch585,
 * official libCH58xBLE.a), with real sleep between radio events (HAL_SLEEP), on the BNK16.
 * Question: does the official stack keep a BLE link through chip sleep on this board (phase 38 lost
 * power or the link), and what does a kept link cost in current?
 *
 * Based on HID_Keyboard/APP/hidkbd_main.c. Changes: pins set the way the BNK16 shutdown leaves them
 * (the example pulls every pin up, which draws current against the closed contacts), haptic boot
 * pattern, own static address (so the Mac keeps the bond of the normal firmware apart), no UART.
 * The app (hidkbd.c) types one letter every BLETEST_REPORT_MS and buzzes, see there.
 *
 * Haptic feedback (haptic switch on):
 *   boot:        N long buzzes, N = reset flag + 1 (2 = real power on, see tests/sleeptest)
 *   connected:   one long buzz
 *   each letter: one short buzz
 *   link lost:   two short buzzes, a pause, then 1-5 short for the reason (see hidkbd.c); it then
 *                advertises again and reconnects by itself
 * Only the boot pattern is a busy wait (before the stack starts); the rest is queued (haptic.c).
 */

#include "CONFIG.h"
#include "HAL.h"
#include "hiddev.h"
#include "hidkbd.h"
#include "bnk16_test.h"
#include "haptic.h"

__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
/* static random address (top two bits set), not the chip MAC that the normal firmware derives its own from */
const uint8_t MacAddr[6] = {0xD5, 0x16, 0xB5, 0x5E, 0x00, 0x01};
#endif

__HIGH_CODE
__attribute__((noinline))
void Main_Circulation()
{
    while(1)
    {
        TMOS_SystemProcess();
    }
}

int main(void)
{
    uint8_t reset_flag = R8_RESET_STATUS & RB_RESET_FLAG;

    HSECFG_Capacitance(HSECap_18p);
    SetSysClock(SYSCLK_FREQ);
    pins_board_init();
    pins_measure_init();

    mDelaymS(500);
    for(uint8_t i = 0; i <= reset_flag; i++)
    {
        buzz(BUZZ_LONG);
        mDelaymS(400);
    }

    CH58x_BLEInit();
    HAL_Init();
    haptic_init();
    GAPRole_PeripheralInit();
    HidDev_Init();
    HidEmu_Init();
    Main_Circulation();
}
