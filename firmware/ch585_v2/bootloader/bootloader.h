#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Layout and IDs come from layout.cmake via -D */
#define APP_START   ((uint32_t)(BL_APP_START))
#define FLASH_END   ((uint32_t)(BL_FLASH_SIZE))
#define UF2_FAMILY  ((uint32_t)(BL_UF2_FAMILY))

/* Binepad stock firmware, as its UF2 updates lay it out: an MCUboot image in the primary slot at
 * 0x18000 (header magic, header size at offset 8), code right after the header (0x19000). It is booted
 * without checking Binepad's signature. */
#define STOCK_UF2_FAMILY   0x1AA143C7u
#define STOCK_SLOT         0x18000u
#define STOCK_IMAGE_MAGIC  0x96F3B83Du

#define FLASH_PAGE   256u   /* write unit, and the UF2 payload size we accept */
#define FLASH_BLOCK  4096u  /* erase unit */

/* One erase block just below the firmware, owned by the bootloader. While an update runs it holds
 *   page 0: "update started" record, written before anything of the firmware is erased
 *   page 1: "update complete" record (image start, block count, CRC), written after the last page
 * so a power cut at any moment can never leave a half-written image that looks bootable. A board
 * flashed through wchisp has this block erased and boots as before ("legacy"). */
#define STATE_ADDR   ((uint32_t)(BL_STATE_ADDR))

#define IMAGE_STATE_LEGACY  0xFFFFFFFFu /* no update ever ran: judge the firmware by its first word */
#define IMAGE_STATE_INVALID 0x00000000u /* an update started and never completed */
/* otherwise: start address of the image the last complete update wrote (APP_START or STOCK_SLOT) */
uint32_t image_state(void);

/* Value the firmware writes to R8_GLOB_RESET_KEEP before a software reset to ask for the bootloader
 * (BOOTLOADER_BOOT_MODE_IAP in qmk_porting/platforms/ch58x/bootloader.h) */
#define RESET_KEEP_ENTER_BOOTLOADER 0x05

/* usb_msc.c */
void usb_msc_init(void);
bool usb_msc_poll(void);   /* true if the host did something (transfer or bus reset) */
bool usb_msc_idle(void);

/* ghostfat.c: the virtual FAT16 drive */
#define DISK_SECTOR_SIZE 512u
uint32_t disk_sector_count(void);
void disk_read(uint32_t lba, uint8_t *buf);
/* false if the sector was a UF2 block that could not be flashed (reported to the host as a write error) */
bool disk_write(uint32_t lba, const uint8_t *buf);

/* true once a complete image was written; main() then reboots into it */
bool uf2_update_done(void);
