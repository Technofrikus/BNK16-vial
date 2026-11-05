/*
 * Virtual FAT16 drive and UF2 flashing.
 *
 * Nothing of the drive is stored: the boot sector, FAT and root directory are generated on every read,
 * and writes are only looked at for UF2 blocks. Each UF2 block carries its own flash address, so it does
 * not matter where the host puts the file on the "disk".
 *
 * Safe against interrupted updates: before anything of the firmware is erased, a "begin" record is
 * written to the state block below APP_START; the firmware's start page (its reset jump, or the MCUboot
 * header of a stock image) is held in RAM and written last, followed by a CRC-protected "done" record.
 * Until that record is valid the bootloader sees no firmware and stays in update mode, whatever
 * moment the power goes away. A different file of the same size, or a retry after an error, restarts
 * the update instead of being merged into it.
 *
 * Two kinds of UF2 are accepted: ours (family BL_UF2_FAMILY, firmware at APP_START) and Binepad's stock
 * updates (family 0x1aa143c7, MCUboot image at 0x18000). Of the area below APP_START only the state block is ever written.
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
    "Binepad stock UF2 files are accepted too (not signature-checked).\r\n"
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

/* ---------- update state (power-fail safety) ---------- */

#define STATE_MAGIC_BEGIN 0xB16B0A55u
#define STATE_MAGIC_DONE  0xB16B0D0Eu
#define STATE_BEGIN_ADDR  (STATE_ADDR)
#define STATE_DONE_ADDR   (STATE_ADDR + FLASH_PAGE)

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
        }
    }
    return ~crc;
}

/* Judged at every boot, see IMAGE_STATE_* in bootloader.h. An update that was cut short, even in the
 * middle of an erase or a page write, leaves either no "begin" record (firmware untouched, legacy
 * check) or a begin record without a valid "done" record (stay in the bootloader). */
uint32_t image_state(void)
{
    if (*(volatile const uint32_t *)STATE_BEGIN_ADDR == 0xFFFFFFFFu) {
        return IMAGE_STATE_LEGACY;
    }
    uint32_t rec[5];
    for (int i = 0; i < 5; i++) {
        rec[i] = ((volatile const uint32_t *)STATE_DONE_ADDR)[i];
    }
    if (rec[0] != STATE_MAGIC_DONE || crc32((const uint8_t *)rec, 16) != rec[4]) {
        return IMAGE_STATE_INVALID;
    }
    if (rec[2] != APP_START && rec[2] != STOCK_SLOT) {
        return IMAGE_STATE_INVALID;
    }
    return rec[2];
}

/* ---------- UF2 update ---------- */

#define APP_SIZE    (FLASH_END - APP_START)
#define MAX_BLOCKS  (APP_SIZE / FLASH_PAGE)
#define APP_ERASE_BLOCKS (APP_SIZE / FLASH_BLOCK)

static struct {
    bool active;                               /* an update has started (state written, start pages erased) */
    bool done;
    bool failed;
    uint32_t num_blocks;
    uint32_t family;
    uint32_t image_start;                      /* page written last */
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

/* true if the flash page at addr already holds exactly data */
__HIGH_CODE static bool flash_page_equals(uint32_t addr, const uint8_t *data)
{
    static __attribute__((aligned(4))) uint8_t tmp[FLASH_PAGE];
    memcpy(tmp, data, FLASH_PAGE);
    return FLASH_ROM_VERIFY(addr, tmp, FLASH_PAGE) == 0;
}

/* Write one state record page: words[0..n) followed by 0xFF */
static bool write_state_page(uint32_t addr, const uint32_t *words, uint32_t n)
{
    static __attribute__((aligned(4))) uint8_t page[FLASH_PAGE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, words, n * 4);
    return flash_page(addr, page);
}

/* Start of a new update. Order matters: the state block is erased and the "begin" record written
 * first, only then the firmware's start pages go. */
static bool begin_update(void)
{
    if (FLASH_ROM_ERASE(STATE_ADDR, FLASH_BLOCK) != 0) {
        return false;
    }
    const uint32_t begin = STATE_MAGIC_BEGIN;
    if (!write_state_page(STATE_BEGIN_ADDR, &begin, 1)) {
        return false;
    }
    /* Invalidate both firmware kinds. The state already says "incomplete", this also keeps the
     * legacy check (wchisp-flashed boards without records) from seeing old code. */
    return ensure_erased(APP_START) && ensure_erased(STOCK_SLOT);
}

/* The last page of an update: the image's start page, then the "done" record. */
static bool commit_update(void)
{
    if (!flash_page(st.image_start, st.first_page)) {
        return false;
    }
    uint32_t rec[5] = {STATE_MAGIC_DONE, st.family, st.image_start, st.num_blocks, 0};
    rec[4] = crc32((const uint8_t *)rec, 16);
    return write_state_page(STATE_DONE_ADDR, rec, 5);
}

/* Is this a plausible start page for the image kind? Catches a file of the right family with the
 * wrong content before it is made bootable. */
static bool start_page_ok(uint32_t image_start, const uint8_t *d)
{
    uint32_t w = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 | (uint32_t)d[3] << 24;
    if (image_start == STOCK_SLOT) {
        uint32_t hdr_size = (uint32_t)d[8] | (uint32_t)d[9] << 8;
        return w == STOCK_IMAGE_MAGIC && hdr_size >= 32 && hdr_size <= 0x1000 && (hdr_size % 4) == 0;
    }
    /* our firmware starts with a jump to its startup code: jal (32 bit) or c.j (16 bit) */
    return (w & 0x7F) == 0x6F || ((w & 3) == 1 && ((w >> 13) & 7) == 5);
}

/* Handle a sector the host wrote. Returns false if it was a UF2 block of the running update and the
 * update has failed (so the host sees a write error instead of a silent success). */
static bool handle_uf2_block(const uf2_block_t *b)
{
    if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 ||
        b->magic_end != UF2_MAGIC_END) {
        return true;                           /* not a UF2 block: FAT, directory, other files */
    }
    if (b->flags & UF2_FLAG_NOFLASH) {
        return true;
    }
    if (!(b->flags & UF2_FLAG_FAMILY_ID)) {
        return true;
    }
    /* Each image has a start page that is written last, so a half-written image never boots:
     * ours starts at APP_START, Binepad's stock image with its MCUboot header at STOCK_SLOT. */
    uint32_t image_start;
    if (b->family_id == UF2_FAMILY) {
        image_start = APP_START;
    } else if (b->family_id == STOCK_UF2_FAMILY) {
        image_start = STOCK_SLOT;
    } else {
        return true;                           /* not for this board */
    }
    if (b->payload_size != FLASH_PAGE || (b->target_addr % FLASH_PAGE) != 0 ||
        b->target_addr < APP_START || b->target_addr + FLASH_PAGE > FLASH_END) {
        return true;                           /* never touch the bootloader itself */
    }
    if (b->num_blocks == 0 || b->num_blocks > MAX_BLOCKS || b->block_no >= b->num_blocks) {
        return true;
    }
    if (st.done) {
        return true;                           /* finished, the board is about to restart */
    }

    bool fresh = !st.active || b->num_blocks != st.num_blocks || b->family_id != st.family;

    if (!fresh && bit_get(st.seen, b->block_no)) {
        /* a retry of a block we have (same content), or a different file with the same size and
         * family (different content): that one must not be merged into the update in progress */
        bool same;
        if (b->target_addr == st.image_start) {
            same = st.first_page_held && memcmp(st.first_page, b->data, FLASH_PAGE) == 0;
        } else {
            same = flash_page_equals(b->target_addr, b->data);
        }
        if (same) {
            return !st.failed;
        }
        fresh = true;
    }
    if (!fresh && st.failed) {
        /* a failed update can be retried without replugging: block 0 starts it over */
        if (b->block_no != 0) {
            return false;
        }
        fresh = true;
    }

    if (fresh) {
        memset(&st, 0, sizeof(st));
        st.active = true;
        st.num_blocks = b->num_blocks;
        st.family = b->family_id;
        st.image_start = image_start;
        if (!begin_update()) {
            st.failed = true;
            return false;
        }
    }

    if (!ensure_erased(b->target_addr)) {
        st.failed = true;
        return false;
    }

    if (b->target_addr == st.image_start) {
        if (!start_page_ok(st.image_start, b->data)) {
            st.failed = true;                  /* wrong content: refuse to make it bootable */
            return false;
        }
        memcpy(st.first_page, b->data, FLASH_PAGE);
        st.first_page_held = true;
    } else if (!flash_page(b->target_addr, b->data)) {
        st.failed = true;
        return false;
    }

    bit_set(st.seen, b->block_no);
    st.received++;

    if (st.received == st.num_blocks) {
        /* everything is in: writing the start page and the done record makes the firmware bootable */
        if (!st.first_page_held || !commit_update()) {
            st.failed = true;
            return false;
        }
        st.done = true;
    }
    return true;
}

bool disk_write(uint32_t lba, const uint8_t *buf)
{
    (void)lba;
    return handle_uf2_block((const uf2_block_t *)buf);
}

bool uf2_update_done(void)
{
    return st.done;
}
