#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Layout and IDs come from layout.cmake via -D */
#define APP_START   ((uint32_t)(BL_APP_START))
#define FLASH_END   ((uint32_t)(BL_FLASH_SIZE))
#define UF2_FAMILY  ((uint32_t)(BL_UF2_FAMILY))

#define FLASH_PAGE   256u   /* write unit, and the UF2 payload size we accept */
#define FLASH_BLOCK  4096u  /* erase unit */

/* Value the firmware writes to R8_GLOB_RESET_KEEP before a software reset to ask for the bootloader
 * (BOOTLOADER_BOOT_MODE_IAP in qmk_porting/platforms/ch58x/bootloader.h) */
#define RESET_KEEP_ENTER_BOOTLOADER 0x05

/* usb_msc.c */
void usb_msc_init(void);
void usb_msc_poll(void);
bool usb_msc_idle(void);

/* ghostfat.c: the virtual FAT16 drive */
#define DISK_SECTOR_SIZE 512u
uint32_t disk_sector_count(void);
void disk_read(uint32_t lba, uint8_t *buf);
void disk_write(uint32_t lba, const uint8_t *buf);

/* true once a complete image was written; main() then reboots into it */
bool uf2_update_done(void);
