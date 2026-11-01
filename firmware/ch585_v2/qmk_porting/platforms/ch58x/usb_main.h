#pragma once

#include "keyboard.h"
#include "gpio.h"
#include "bootloader.h"

#define USB_PERIODICAL_BIOS_REPORT_EVT 0x0002

void usb_task_init();
void usb_run_qmk_task(void);
void usb_start_periodical_bios_report();
