/*
 * Virtual FAT16 drive and UF2 flashing.
 *
 * Nothing of the drive is stored: the boot sector, FAT and root directory are generated on every read,
 * and writes are only looked at for UF2 blocks. Each UF2 block carries its own flash address, so it does
 * not matter where the host puts the file on the "disk".
 *
 * Safe against interrupted updates: the first flash page of the firmware (its reset jump) is erased
 * by the first accepted block and written back only after all blocks of the file have arrived. Until
 * then the bootloader sees no firmware and stays in update mode on the next reset.
 */

#include <string.h>
#include "CH58x_common.h"
#include "bootloader.h"

/* ---------- disk geometry ---------- */

#define TOTAL_SECTORS    16384u   /* 8 MB, enough for FAT16 (needs >= 4085 clusters) */
#define RESERVED_SECTORS 1u
#define NUM_FATS         2u
#define SECTORS_PER_FAT  64u      /* 64 * 256 entries >= clusters + 2 */
#define ROOT_ENTRIES     64u
#define ROOT_SECTORS     (ROOT_ENTRIES * 32u / DISK_SECTOR_SIZE)

#define START_FAT0     RESERVED_SECTORS
#define START_FAT1     (START_FAT0 + SECTORS_PER_FAT)
#define START_ROOT     (START_FAT1 + SECTORS_PER_FAT)
#define START_CLUSTERS (START_ROOT + ROOT_SECTORS)

#define VOLUME_LABEL "BNK16BOOT  " /* 11 chars, space padded */

static const char info_txt[] =
    "UF2 Bootloader " BL_VERSION "\r\n"
    "Model: Binepad BNK16 (BNK16-vial)\r\n"
    "Board-ID: CH585-BNK16\r\n"
    "\r\n"
    "Copy a bnk16-*.uf2 file onto this drive to update the firmware.\r\n"
    "The board restarts by itself when the copy is done.\r\n";

static const char index_htm[] =
    "<!doctype html>\n<html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"refresh\" content=\"0; url=https://github.com/Technofrikus/BNK16-vial/releases\">"
    "</head><body><a href=\"https://github.com/Technofrikus/BNK16-vial/releases\">BNK16-vial releases</a>"
    "</body></html>\n";

struct file_entry {
    const char name[11];
    const char *data;
    uint32_t size;
};

static const struct file_entry files[] = {
    {"INFO_UF2TXT", info_txt, sizeof(info_txt) - 1},
    {"INDEX   HTM", index_htm, sizeof(index_htm) - 1},
};
#define NUM_FILES (sizeof(files) / sizeof(files[0]))
/* every file fits in one cluster (one sector): file i lives in cluster 2 + i */
_Static_assert(sizeof(info_txt) - 1 <= DISK_SECTOR_SIZE, "INFO_UF2.TXT too large");
_Static_assert(sizeof(index_htm) - 1 <= DISK_SECTOR_SIZE, "INDEX.HTM too large");

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

uint32_t disk_sector_count(void)
{
    return TOTAL_SECTORS;
}

static void make_boot_sector(uint8_t *b)
{
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, DISK_SECTOR_SIZE);
    b[13] = 1;                                 /* sectors per cluster */
    put16(b + 14, RESERVED_SECTORS);
    b[16] = NUM_FATS;
    put16(b + 17, ROOT_ENTRIES);
    put16(b + 19, TOTAL_SECTORS);
    b[21] = 0xF8;                              /* media: fixed disk */
    put16(b + 22, SECTORS_PER_FAT);
    put16(b + 24, 1);                          /* sectors per track */
    put16(b + 26, 1);                          /* heads */
    b[36] = 0x80;                              /* drive number */
    b[38] = 0x29;                              /* extended boot signature */
    put32(b + 39, 0xB16B0075);                 /* volume serial */
    memcpy(b + 43, VOLUME_LABEL, 11);
    memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
}

static void make_root_dir(uint8_t *b)
{
    /* entry 0: volume label */
    memcpy(b, VOLUME_LABEL, 11);
    b[11] = 0x08;
    for (uint32_t i = 0; i < NUM_FILES; i++) {
        uint8_t *e = b + 32 * (i + 1);
        memcpy(e, files[i].name, 11);
        e[11] = 0x01;                          /* read-only */
        put16(e + 24, (1 << 5) | 1);           /* date: 1980-01-01 */
        put16(e + 18, (1 << 5) | 1);           /* last access date */
        put16(e + 16, (1 << 5) | 1);           /* creation date */
        put16(e + 26, 2 + i);                  /* first cluster */
        put32(e + 28, files[i].size);
    }
}

void disk_read(uint32_t lba, uint8_t *buf)
{
    memset(buf, 0, DISK_SECTOR_SIZE);

    if (lba == 0) {
        make_boot_sector(buf);
    } else if (lba >= START_FAT0 && lba < START_ROOT) {
        uint32_t fat_sector = (lba - START_FAT0) % SECTORS_PER_FAT;
        if (fat_sector == 0) {
            put16(buf + 0, 0xFFF8);            /* media descriptor */
            put16(buf + 2, 0xFFFF);
            for (uint32_t i = 0; i < NUM_FILES; i++) {
                put16(buf + 4 + 2 * i, 0xFFFF); /* end of chain: each file is one cluster */
            }
        }
    } else if (lba >= START_ROOT && lba < START_CLUSTERS) {
        if (lba == START_ROOT) {
            make_root_dir(buf);
        }
    } else if (lba >= START_CLUSTERS) {
        uint32_t idx = lba - START_CLUSTERS;
        if (idx < NUM_FILES) {
            memcpy(buf, files[idx].data, files[idx].size);
        }
    }
}

/* ---------- UF2 ---------- */

#define UF2_MAGIC_START0 0x0A324655u
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u
#define UF2_FLAG_NOFLASH     0x00000001u
#define UF2_FLAG_FAMILY_ID   0x00002000u

typedef struct {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t family_id;
    uint8_t data[476];
    uint32_t magic_end;
} uf2_block_t;
_Static_assert(sizeof(uf2_block_t) == 512, "UF2 block must be 512 bytes");

#define APP_SIZE    (FLASH_END - APP_START)
#define MAX_BLOCKS  (APP_SIZE / FLASH_PAGE)
#define APP_ERASE_BLOCKS (APP_SIZE / FLASH_BLOCK)

static struct {
    bool active;                               /* an update has started (first page erased) */
    bool done;
    bool failed;
    uint32_t num_blocks;
    uint32_t received;
    uint8_t seen[(MAX_BLOCKS + 7) / 8];
    uint8_t erased[(APP_ERASE_BLOCKS + 7) / 8];
    bool first_page_held;
    __attribute__((aligned(4))) uint8_t first_page[FLASH_PAGE];
} st;

static bool bit_get(const uint8_t *map, uint32_t i) { return map[i >> 3] & (1u << (i & 7)); }
static void bit_set(uint8_t *map, uint32_t i) { map[i >> 3] |= 1u << (i & 7); }

/* flash programming runs from RAM, as in WCH's IAP examples */
__HIGH_CODE static bool ensure_erased(uint32_t addr)
{
    uint32_t idx = (addr - APP_START) / FLASH_BLOCK;
    if (bit_get(st.erased, idx)) {
        return true;
    }
    if (FLASH_ROM_ERASE(APP_START + idx * FLASH_BLOCK, FLASH_BLOCK) != 0) {
        return false;
    }
    bit_set(st.erased, idx);
    return true;
}

__HIGH_CODE static bool flash_page(uint32_t addr, const uint8_t *data)
{
    /* FLASH_ROM_WRITE wants a word-aligned RAM buffer */
    static __attribute__((aligned(4))) uint8_t tmp[FLASH_PAGE];
    memcpy(tmp, data, FLASH_PAGE);
    if (FLASH_ROM_WRITE(addr, tmp, FLASH_PAGE) != 0) {
        return false;
    }
    return FLASH_ROM_VERIFY(addr, tmp, FLASH_PAGE) == 0;
}

static void handle_uf2_block(const uf2_block_t *b)
{
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 ||
        b->magic_end != UF2_MAGIC_END) {
        return;
    }
    if (b->flags & UF2_FLAG_NOFLASH) {
        return;
    }
    if (!(b->flags & UF2_FLAG_FAMILY_ID) || b->family_id != UF2_FAMILY) {
        return;                                /* not for this board, e.g. a Binepad stock UF2 */
    }
    if (b->payload_size != FLASH_PAGE || (b->target_addr % FLASH_PAGE) != 0 ||
        b->target_addr < APP_START || b->target_addr + FLASH_PAGE > FLASH_END) {
        return;                                /* never touch the bootloader itself */
    }
    if (b->num_blocks == 0 || b->num_blocks > MAX_BLOCKS || b->block_no >= b->num_blocks) {
        return;
    }
    if (st.done || st.failed) {
        return;
    }

    if (!st.active || b->num_blocks != st.num_blocks) {
        /* new update: invalidate the current firmware right away */
        memset(&st, 0, sizeof(st));
        st.active = true;
        st.num_blocks = b->num_blocks;
        if (!ensure_erased(APP_START)) {
            st.failed = true;
            return;
        }
    }
    if (bit_get(st.seen, b->block_no)) {
        return;                                /* host retried a block */
    }
    if (!ensure_erased(b->target_addr)) {
        st.failed = true;
        return;
    }

    if (b->target_addr == APP_START) {
        memcpy(st.first_page, b->data, FLASH_PAGE);
        st.first_page_held = true;
    } else if (!flash_page(b->target_addr, b->data)) {
        st.failed = true;
        return;
    }

    bit_set(st.seen, b->block_no);
    st.received++;

    if (st.received == st.num_blocks) {
        /* everything is in: writing the first page makes the firmware bootable */
        if (!st.first_page_held || !flash_page(APP_START, st.first_page)) {
            st.failed = true;
            return;
        }
        st.done = true;
    }
}

void disk_write(uint32_t lba, const uint8_t *buf)
{
    (void)lba;
    handle_uf2_block((const uf2_block_t *)buf);
}

bool uf2_update_done(void)
{
    return st.done;
}
