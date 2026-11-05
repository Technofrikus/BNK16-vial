/*
 * USB mass storage (bulk-only transport, SCSI) on the CH585 high-speed USB controller.
 *
 * Polled from the main loop, no interrupts, so flash writes simply pause USB (the host sees NAKs).
 * Register handling follows WCH's CH585 USBHS examples (user/usb/ch585_usbhs_device.c in this repo and
 * EVT/EXAM/USB/USBHS/DEVICE/MSC_U-Disk in openwch/ch585).
 *
 * Endpoints: EP0 control (64 bytes), EP1 IN + EP1 OUT bulk (512 bytes high speed, 64 full speed).
 */

#include <string.h>
#include "CH58x_common.h"
#include "bootloader.h"

#define USB_VID 0x5650
#define USB_PID 0x0B1B /* BNK16 firmware is 0x0B16 */

#define EP0_SIZE 64u
#define BULK_HS  512u
#define BULK_FS  64u

/* ---------- descriptors ---------- */

static const uint8_t dev_desc[18] = {
    18, USB_DESCR_TYP_DEVICE, 0x00, 0x02, /* USB 2.0 */
    0x00, 0x00, 0x00,                     /* class per interface */
    EP0_SIZE,
    USB_VID & 0xFF, USB_VID >> 8,
    USB_PID & 0xFF, USB_PID >> 8,
    0x00, 0x01,                           /* bcdDevice 1.00 */
    1, 2, 3,                              /* manufacturer, product, serial strings */
    1,
};

static uint8_t cfg_desc[32] = {
    /* configuration */
    9, USB_DESCR_TYP_CONFIG, 32, 0, 1, 1, 0, 0x80, 50,
    /* interface: mass storage, SCSI transparent, bulk-only */
    9, USB_DESCR_TYP_INTERF, 0, 0, 2, 0x08, 0x06, 0x50, 0,
    /* EP1 IN bulk */
    7, USB_DESCR_TYP_ENDP, 0x81, 0x02, BULK_HS & 0xFF, BULK_HS >> 8, 0,
    /* EP1 OUT bulk */
    7, USB_DESCR_TYP_ENDP, 0x01, 0x02, BULK_HS & 0xFF, BULK_HS >> 8, 0,
};

static const uint8_t qualifier_desc[10] = {
    10, USB_DESCR_TYP_QUALIF, 0x00, 0x02, 0x00, 0x00, 0x00, EP0_SIZE, 1, 0,
};

static const uint8_t lang_desc[4] = {4, USB_DESCR_TYP_STRING, 0x09, 0x04};
/* serial number: 12 hex digits of the chip's unique ID (filled in by usb_msc_init), so several boards in
 * bootloader mode at the same time are told apart. USB MSC wants at least 12 digits. */
static char serial_str[13] = "BNK16BOOT";
static const char *const strings[] = {NULL, "BNK16-vial", "BNK16 UF2 Bootloader", serial_str};

/* ---------- buffers ---------- */

static __attribute__((aligned(4))) uint8_t ep0_buf[EP0_SIZE];
static __attribute__((aligned(4))) uint8_t ep1_rx_buf[BULK_HS];
static __attribute__((aligned(4))) uint8_t ep1_tx_buf[BULK_HS];
static __attribute__((aligned(4))) uint8_t sector[DISK_SECTOR_SIZE];
static uint8_t str_buf[2 + 2 * 32];
static uint8_t other_cfg_desc[32];

/* ---------- control transfer state ---------- */

static uint8_t setup_type, setup_req;
static uint16_t setup_value;
static const uint8_t *ctrl_ptr;
static uint16_t ctrl_left;
static bool ctrl_zlp;
static uint8_t dev_addr, dev_config;
static uint16_t bulk_size = BULK_HS;

/* ---------- bulk-only transport state ---------- */

enum { BOT_CBW, BOT_DATA_IN, BOT_DATA_OUT, BOT_CSW, BOT_WAIT_CLEAR };

static uint8_t bot_state;
static uint32_t cbw_tag, cbw_len, bot_left;
static uint8_t csw_status;
static uint8_t csw_clear_ep;                 /* endpoint (0x81 / 0x01) whose halt the host must clear before the CSW, else 0 */
static bool cbw_dir_in;                      /* data direction announced by the CBW */
static bool write_failed;                    /* a sector of the current WRITE could not be flashed */

static bool disk_xfer;                       /* data stage reads/writes disk sectors */
static uint32_t xfer_lba, xfer_off, xfer_left;
static uint8_t resp[36];
static uint8_t resp_len, resp_pos;

static uint8_t sense_key, sense_asc;

/* ---------- endpoint helpers ---------- */

static void ep1_reset(void)
{
    R32_U2EP1_MAX_LEN = BULK_HS;
    R32_U2EP1_RX_DMA = (uint32_t)ep1_rx_buf;
    R32_U2EP1_TX_DMA = (uint32_t)ep1_tx_buf;
    R16_U2EP1_T_LEN = 0;
    R8_U2EP1_TX_CTRL = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
    R8_U2EP1_RX_CTRL = USBHS_UEP_R_TOG_DATA0 | USBHS_UEP_R_RES_ACK;
}

static void endpoints_init(void)
{
    R16_U2EP_TX_EN = RB_EP0_EN | RB_EP1_EN;
    R16_U2EP_RX_EN = RB_EP0_EN | RB_EP1_EN;
    R32_U2EP0_MAX_LEN = EP0_SIZE;
    R32_U2EP0_DMA = (uint32_t)ep0_buf;
    R8_U2EP0_TX_CTRL = USBHS_UEP_T_RES_NAK;
    R8_U2EP0_RX_CTRL = USBHS_UEP_R_RES_ACK;
    ep1_reset();
}

static void ep1_out_set(uint8_t res)
{
    R8_U2EP1_RX_CTRL = (R8_U2EP1_RX_CTRL & ~USBHS_UEP_R_RES_MASK) | res;
}

static void ep1_in_send(uint16_t len)
{
    R16_U2EP1_T_LEN = len;
    R8_U2EP1_TX_CTRL = (R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
}

/* ---------- bulk-only transport ---------- */

static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get_be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void send_csw(void)
{
    put_le32(ep1_tx_buf, 0x53425355);        /* "USBS" */
    put_le32(ep1_tx_buf + 4, cbw_tag);
    put_le32(ep1_tx_buf + 8, bot_left);       /* residue */
    ep1_tx_buf[12] = csw_status;
    bot_state = BOT_CSW;
    ep1_in_send(13);
}

static void send_next_in(void)
{
    uint32_t chunk;

    if (disk_xfer) {
        if (xfer_off == 0) {
            disk_read(xfer_lba, sector);
        }
        chunk = DISK_SECTOR_SIZE - xfer_off;
        if (chunk > bulk_size) chunk = bulk_size;
        if (chunk > xfer_left) chunk = xfer_left;
        memcpy(ep1_tx_buf, sector + xfer_off, chunk);
        xfer_left -= chunk;
        xfer_off += chunk;
        if (xfer_off >= DISK_SECTOR_SIZE) {
            xfer_off = 0;
            xfer_lba++;
        }
    } else {
        chunk = resp_len - resp_pos;
        if (chunk > bulk_size) chunk = bulk_size;
        memcpy(ep1_tx_buf, resp + resp_pos, chunk);
        resp_pos += chunk;
    }
    bot_left -= chunk;
    ep1_in_send(chunk);
}

/* The host expects (or still sends) data we will not transfer: stall that endpoint, the CSW follows
 * once the host clears the halt. Also the BOT answer when less data than announced was moved and the
 * last packet was full size, so the host cannot tell the transfer has ended. */
static void stall_then_csw(bool data_in)
{
    bot_state = BOT_WAIT_CLEAR;
    if (data_in) {
        csw_clear_ep = 0x81;
        R8_U2EP1_TX_CTRL = (R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_STALL;
    } else {
        csw_clear_ep = 0x01;
        ep1_out_set(USBHS_UEP_R_RES_STALL);
    }
}

static void fail_command(uint8_t key, uint8_t asc, bool data_in)
{
    sense_key = key;
    sense_asc = asc;
    csw_status = 1;
    if (cbw_len == 0) {
        bot_left = 0;
        send_csw();
        return;
    }
    bot_left = cbw_len;
    stall_then_csw(data_in);
}

/* All data of an IN command is sent. Less than announced and ending on a full packet needs a stall,
 * otherwise the host would wait for more data. */
static void data_in_finished(void)
{
    uint32_t sent = cbw_len - bot_left;
    if (bot_left != 0 && (sent % bulk_size) == 0) {
        stall_then_csw(true);
    } else {
        send_csw();
    }
}

/* Short fixed response to a command; bot_left counts down from what the host asked for and ends as the
 * CSW residue. */
static void respond(const uint8_t *data, uint8_t len)
{
    if (len > sizeof(resp)) len = sizeof(resp);
    if (len > cbw_len) len = (uint8_t)cbw_len;
    if (len) memcpy(resp, data, len);
    resp_len = len;
    resp_pos = 0;
    disk_xfer = false;
    csw_status = 0;
    bot_left = cbw_len;
    if (resp_len == 0) {
        if (bot_left == 0) {
            send_csw();
        } else {
            stall_then_csw(cbw_dir_in);        /* host wanted data for a command without any */
        }
        return;
    }
    bot_state = BOT_DATA_IN;
    send_next_in();
}

static void scsi_command(const uint8_t *cb, bool dir_in)
{
    uint8_t buf[36];

    switch (cb[0]) {
        case 0x00: /* TEST UNIT READY */
        case 0x1E: /* PREVENT ALLOW MEDIUM REMOVAL */
        case 0x1B: /* START STOP UNIT */
        case 0x2F: /* VERIFY(10) */
        case 0x35: /* SYNCHRONIZE CACHE(10) */
            respond(NULL, 0);
            return;

        case 0x12: /* INQUIRY */
            if ((cb[1] & 1) || cb[2]) {      /* VPD pages are not supported */
                fail_command(0x05, 0x24, dir_in); /* ILLEGAL REQUEST, invalid field in CDB */
                return;
            }
            memset(buf, 0, sizeof(buf));
            buf[1] = 0x80;                   /* removable */
            buf[2] = 0x02;
            buf[3] = 0x02;
            buf[4] = 31;
            memcpy(buf + 8, "BNK16   ", 8);
            memcpy(buf + 16, "UF2 Bootloader  ", 16);
            memcpy(buf + 32, "1.0 ", 4);
            respond(buf, 36);
            return;

        case 0x03: /* REQUEST SENSE */
            memset(buf, 0, 18);
            buf[0] = 0x70;
            buf[2] = sense_key;
            buf[7] = 10;
            buf[12] = sense_asc;
            sense_key = sense_asc = 0;
            respond(buf, 18);
            return;

        case 0x25: /* READ CAPACITY(10) */
            put_be32(buf, disk_sector_count() - 1);
            put_be32(buf + 4, DISK_SECTOR_SIZE);
            respond(buf, 8);
            return;

        case 0x23: /* READ FORMAT CAPACITIES */
            memset(buf, 0, 12);
            buf[3] = 8;
            put_be32(buf + 4, disk_sector_count());
            buf[8] = 0x02;                   /* formatted media */
            buf[10] = DISK_SECTOR_SIZE >> 8;
            buf[11] = DISK_SECTOR_SIZE & 0xFF;
            respond(buf, 12);
            return;

        case 0x1A: /* MODE SENSE(6) */
            memset(buf, 0, 4);
            buf[0] = 3;
            respond(buf, 4);
            return;

        case 0x5A: /* MODE SENSE(10) */
            memset(buf, 0, 8);
            buf[1] = 6;
            respond(buf, 8);
            return;

        case 0x28:   /* READ(10) */
        case 0x2A: { /* WRITE(10) */
            uint32_t lba = get_be32(cb + 2);
            uint32_t count = (uint32_t)cb[7] << 8 | cb[8];
            uint32_t bytes = count * DISK_SECTOR_SIZE;
            bool is_read = cb[0] == 0x28;

            uint32_t total = disk_sector_count();
            if (count > total || lba > total - count) { /* written so that lba + count cannot wrap */
                fail_command(0x05, 0x21, is_read); /* ILLEGAL REQUEST, LBA out of range */
                return;
            }
            if (bytes > cbw_len) bytes = cbw_len;
            disk_xfer = true;
            xfer_lba = lba;
            xfer_off = 0;
            xfer_left = bytes;
            csw_status = 0;
            write_failed = false;
            bot_left = cbw_len;
            if (bytes == 0) {
                if (bot_left == 0) {
                    send_csw();
                } else {
                    stall_then_csw(is_read);
                }
                return;
            }
            if (is_read) {
                bot_state = BOT_DATA_IN;
                send_next_in();
            } else {
                bot_state = BOT_DATA_OUT;
                ep1_out_set(USBHS_UEP_R_RES_ACK);
            }
            return;
        }

        default:
            fail_command(0x05, 0x20, dir_in); /* ILLEGAL REQUEST, invalid command */
            return;
    }
}

static void bot_out_packet(const uint8_t *data, uint16_t len)
{
    if (bot_state == BOT_CBW) {
        uint8_t cb_len = data[14] & 0x1F;
        if (len != 31 || get_le32(data) != 0x43425355 || cb_len < 1 || cb_len > 16) { /* "USBC" */
            /* invalid CBW: stall both directions until the host resets */
            R8_U2EP1_TX_CTRL = (R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_STALL;
            ep1_out_set(USBHS_UEP_R_RES_STALL);
            return;
        }
        cbw_tag = get_le32(data + 4);
        cbw_len = get_le32(data + 8);
        cbw_dir_in = (data[12] & 0x80) != 0;
        write_failed = false;
        if ((data[13] & 0x0F) != 0) {
            /* we have a single LUN; REQUEST SENSE / INQUIRY would still be allowed for others,
             * but a host that asks for LUN > 0 got it from somewhere else, so refuse */
            fail_command(0x05, 0x25, cbw_dir_in); /* ILLEGAL REQUEST, logical unit not supported */
        } else {
            scsi_command(data + 15, cbw_dir_in);
        }
        if (bot_state == BOT_CBW || bot_state == BOT_DATA_OUT) {
            ep1_out_set(USBHS_UEP_R_RES_ACK);
        }
        return;
    }

    if (bot_state == BOT_DATA_OUT) {
        if (len > xfer_left) len = xfer_left;
        uint32_t pos = 0;
        while (pos < len) {
            uint32_t n = DISK_SECTOR_SIZE - xfer_off;
            if (n > len - pos) n = len - pos;
            memcpy(sector + xfer_off, data + pos, n);
            xfer_off += n;
            pos += n;
            if (xfer_off >= DISK_SECTOR_SIZE) {
                if (!disk_write(xfer_lba++, sector)) {
                    write_failed = true;
                }
                xfer_off = 0;
            }
        }
        xfer_left -= len;
        bot_left -= len;
        if (xfer_left == 0) {
            if (write_failed) {
                sense_key = 0x03;              /* MEDIUM ERROR, write error */
                sense_asc = 0x0C;
                csw_status = 1;
            }
            if (bot_left != 0) {
                stall_then_csw(false);         /* host announced more data than the command moves */
            } else {
                send_csw();
            }
        } else {
            ep1_out_set(USBHS_UEP_R_RES_ACK);
        }
    }
}

static void bot_in_done(void)
{
    if (bot_state == BOT_DATA_IN) {
        bool more = disk_xfer ? xfer_left > 0 : resp_pos < resp_len;
        if (more) {
            send_next_in();
        } else {
            data_in_finished();
        }
    } else if (bot_state == BOT_CSW) {
        bot_state = BOT_CBW;
        ep1_out_set(USBHS_UEP_R_RES_ACK);
    }
}

static void bot_reset(void)
{
    bot_state = BOT_CBW;
    csw_clear_ep = 0;
    write_failed = false;
    bot_left = 0;
    ep1_reset();
}

/* ---------- control endpoint ---------- */

static void ep0_stall(void)
{
    R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_STALL;
    R8_U2EP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_STALL;
}

static void ep0_send_chunk(void)
{
    uint16_t n = ctrl_left > EP0_SIZE ? EP0_SIZE : ctrl_left;
    memcpy(ep0_buf, ctrl_ptr, n);
    ctrl_ptr += n;
    ctrl_left -= n;
    if (n < EP0_SIZE) ctrl_zlp = false;      /* a short packet ends the data stage by itself */
    R16_U2EP0_T_LEN = n;
}

static const uint8_t *string_desc(uint8_t idx, uint16_t *len)
{
    if (idx == 0) {
        *len = sizeof(lang_desc);
        return lang_desc;
    }
    if (idx >= sizeof(strings) / sizeof(strings[0])) {
        return NULL;
    }
    const char *s = strings[idx];
    uint8_t n = 0;
    while (s[n] && n < 32) {
        str_buf[2 + 2 * n] = s[n];
        str_buf[3 + 2 * n] = 0;
        n++;
    }
    str_buf[0] = 2 + 2 * n;
    str_buf[1] = USB_DESCR_TYP_STRING;
    *len = str_buf[0];
    return str_buf;
}

/* The bulk packet size follows the speed the host chose. Looked at whenever it matters (descriptor
 * requests and SET_CONFIGURATION), not once: after a bus reset the speed can differ from last time. */
static void apply_speed(void)
{
    bulk_size = (R8_USB2_MIS_ST & USBHS_UDMS_HS_MOD) ? BULK_HS : BULK_FS;
    cfg_desc[22] = cfg_desc[29] = bulk_size & 0xFF;
    cfg_desc[23] = cfg_desc[30] = bulk_size >> 8;
}

static void handle_setup(void)
{
    PUSB_SETUP_REQ req = (PUSB_SETUP_REQ)ep0_buf;
    uint16_t wlength = req->wLength;
    uint16_t windex = req->wIndex;
    const uint8_t *data = NULL;
    uint16_t len = 0;
    bool ok = true;

    setup_type = req->bRequestType;
    setup_req = req->bRequest;
    setup_value = req->wValue;

    if ((setup_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_CLASS) {
        switch (setup_req) {
            case 0xFE: /* GET MAX LUN */
                ep0_buf[0] = 0;
                data = ep0_buf;
                len = 1;
                break;
            case 0xFF: /* BULK-ONLY MASS STORAGE RESET */
                bot_reset();
                break;
            default:
                ok = false;
        }
    } else if ((setup_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD) {
        switch (setup_req) {
            case USB_GET_DESCRIPTOR:
                switch (setup_value >> 8) {
                    case USB_DESCR_TYP_DEVICE:
                        data = dev_desc;
                        len = sizeof(dev_desc);
                        break;
                    case USB_DESCR_TYP_CONFIG:
                        apply_speed();
                        data = cfg_desc;
                        len = sizeof(cfg_desc);
                        break;
                    case 0x07: /* OTHER_SPEED_CONFIGURATION: same layout, the other speed's packet size */
                        apply_speed();
                        memcpy(other_cfg_desc, cfg_desc, sizeof(cfg_desc));
                        other_cfg_desc[1] = 0x07;
                        other_cfg_desc[22] = other_cfg_desc[29] = (bulk_size == BULK_HS ? BULK_FS : BULK_HS) & 0xFF;
                        other_cfg_desc[23] = other_cfg_desc[30] = (bulk_size == BULK_HS ? BULK_FS : BULK_HS) >> 8;
                        data = other_cfg_desc;
                        len = sizeof(other_cfg_desc);
                        break;
                    case USB_DESCR_TYP_QUALIF:
                        data = qualifier_desc;
                        len = sizeof(qualifier_desc);
                        break;
                    case USB_DESCR_TYP_STRING:
                        data = string_desc(setup_value & 0xFF, &len);
                        ok = data != NULL;
                        break;
                    default:
                        ok = false;
                }
                break;

            case USB_SET_ADDRESS:
                dev_addr = setup_value & 0x7F;
                break;

            case USB_GET_CONFIGURATION:
                ep0_buf[0] = dev_config;
                data = ep0_buf;
                len = 1;
                break;

            case USB_SET_CONFIGURATION:
                if ((setup_value & 0xFF) > 1) {
                    ok = false;                /* only configuration 1 exists */
                    break;
                }
                apply_speed();
                dev_config = setup_value & 0xFF;
                bot_reset();
                break;

            case USB_GET_INTERFACE:
                ep0_buf[0] = 0;
                data = ep0_buf;
                len = 1;
                break;

            case USB_SET_INTERFACE:
                if (setup_value != 0 || (windex & 0xFF) != 0) {
                    ok = false;                /* one interface, no alternate settings */
                }
                break;

            case USB_GET_STATUS:
                ep0_buf[0] = ep0_buf[1] = 0;
                if ((setup_type & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP) {
                    if ((windex & 0xFF) == 0x81 &&
                        (R8_U2EP1_TX_CTRL & USBHS_UEP_T_RES_MASK) == USBHS_UEP_T_RES_STALL) {
                        ep0_buf[0] = 1;
                    }
                    if ((windex & 0xFF) == 0x01 &&
                        (R8_U2EP1_RX_CTRL & USBHS_UEP_R_RES_MASK) == USBHS_UEP_R_RES_STALL) {
                        ep0_buf[0] = 1;
                    }
                }
                data = ep0_buf;
                len = 2;
                break;

            case USB_CLEAR_FEATURE:
                if ((setup_type & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP &&
                    (setup_value & 0xFF) == USB_REQ_FEAT_ENDP_HALT) {
                    if ((windex & 0xFF) == 0x81) {
                        R8_U2EP1_TX_CTRL = USBHS_UEP_T_TOG_DATA0 | USBHS_UEP_T_RES_NAK;
                    } else if ((windex & 0xFF) == 0x01) {
                        /* stay NAK while a CSW is pending: OUT is re-armed once the CSW went out */
                        R8_U2EP1_RX_CTRL = USBHS_UEP_R_TOG_DATA0 |
                                           (bot_state == BOT_WAIT_CLEAR ? USBHS_UEP_R_RES_NAK : USBHS_UEP_R_RES_ACK);
                    } else {
                        ok = false;
                    }
                    /* the CSW goes out once the endpoint that was stalled is cleared */
                    if (ok && csw_clear_ep != 0 && csw_clear_ep == (windex & 0xFF)) {
                        csw_clear_ep = 0;
                        send_csw();
                    }
                } else if ((setup_type & USB_REQ_RECIP_MASK) != USB_REQ_RECIP_DEVICE) {
                    ok = false;
                }
                break;

            case USB_SET_FEATURE:
                if ((setup_type & USB_REQ_RECIP_MASK) == USB_REQ_RECIP_ENDP &&
                    (setup_value & 0xFF) == USB_REQ_FEAT_ENDP_HALT) {
                    if ((windex & 0xFF) == 0x81) {
                        R8_U2EP1_TX_CTRL = (R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_STALL;
                    } else if ((windex & 0xFF) == 0x01) {
                        ep1_out_set(USBHS_UEP_R_RES_STALL);
                    } else {
                        ok = false;
                    }
                }
                break;

            default:
                ok = false;
        }
    } else {
        ok = false;
    }

    if (!ok) {
        ep0_stall();
        return;
    }

    if (setup_type & USB_REQ_TYP_IN) {
        if (len > wlength) len = wlength;
        ctrl_ptr = data;
        ctrl_left = len;
        ctrl_zlp = len < wlength && (len % EP0_SIZE) == 0;
        ep0_send_chunk();
        R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
    } else {
        /* no OUT data stage is supported: answer with the status stage */
        R16_U2EP0_T_LEN = 0;
        R8_U2EP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_ACK;
    }
}

static void ep0_in_done(void)
{
    if (setup_type & USB_REQ_TYP_IN) {
        if (ctrl_left > 0 || ctrl_zlp) {
            if (ctrl_left == 0) {
                ctrl_zlp = false;
                R16_U2EP0_T_LEN = 0;
            } else {
                ep0_send_chunk();
            }
            R8_U2EP0_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
            R8_U2EP0_TX_CTRL = (R8_U2EP0_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_ACK;
        } else {
            /* data stage done: accept the host's zero-length status packet */
            R8_U2EP0_TX_CTRL = (R8_U2EP0_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;
            R8_U2EP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_ACK;
        }
    } else {
        /* status stage of a request without data stage done */
        if ((setup_type & USB_REQ_TYP_MASK) == USB_REQ_TYP_STANDARD && setup_req == USB_SET_ADDRESS) {
            R8_USB2_DEV_AD = dev_addr;
        }
        R8_U2EP0_TX_CTRL = (R8_U2EP0_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;
    }
    R8_U2EP0_TX_CTRL &= ~USBHS_UEP_T_DONE;
}

/* ---------- public ---------- */

void usb_msc_init(void)
{
    uint8_t mac[6];
    uint8_t any = 0, all = 0xFF;
    GetMACAddress(mac);
    for (int i = 0; i < 6; i++) {
        any |= mac[i];
        all &= mac[i];
    }
    if (any != 0 && all != 0xFF) {             /* keep the fixed fallback if the ID was never programmed */
        for (int i = 0; i < 6; i++) {
            serial_str[2 * i] = "0123456789ABCDEF"[mac[i] >> 4];
            serial_str[2 * i + 1] = "0123456789ABCDEF"[mac[i] & 0xF];
        }
        serial_str[12] = 0;
    }

    /* same sequence as the firmware's USBHS_Device_Init(), without enabling the interrupt */
    R16_CLK_SYS_CFG |= (RB_CLK_SYS_MOD & 0x40) | RB_XROM_SCLK_SEL | RB_OSC32M_SEL;
    R8_USBHS_PLL_CTRL = USBHS_PLL_EN;
    R16_PIN_CONFIG |= RB_PIN_USB2_EN;

    R8_USB2_CTRL = USBHS_UD_RST_LINK | USBHS_UD_PHY_SUSPENDM;
    R8_USB2_INT_EN = USBHS_UDIE_BUS_RST | USBHS_UDIE_SUSPEND | USBHS_UDIE_BUS_SLEEP | USBHS_UDIE_LPM_ACT |
                     USBHS_UDIE_TRANSFER | USBHS_UDIE_LINK_RDY;
    endpoints_init();
    R8_USB2_BASE_MODE = USBHS_UD_SPEED_HIGH;
    R8_USB2_CTRL = USBHS_UD_DEV_EN | USBHS_UD_DMA_EN | USBHS_UD_LPM_EN | USBHS_UD_PHY_SUSPENDM;
    bot_reset();
}

bool usb_msc_poll(void)
{
    uint8_t flags = R8_USB2_INT_FG;
    bool activity = (flags & (USBHS_UDIF_TRANSFER | USBHS_UDIF_BUS_RST)) != 0;

    if (flags & USBHS_UDIF_TRANSFER) {
        uint8_t st = R8_USB2_INT_ST;
        uint8_t ep = st & USBHS_UDIS_EP_ID_MASK;

        if (st & USBHS_UDIS_EP_DIR) {
            /* IN transfer completed */
            if (ep == 0) {
                ep0_in_done();
            } else if (ep == 1) {
                R8_U2EP1_TX_CTRL = (R8_U2EP1_TX_CTRL & ~USBHS_UEP_T_RES_MASK) | USBHS_UEP_T_RES_NAK;
                R8_U2EP1_TX_CTRL ^= USBHS_UEP_T_TOG_DATA1;
                R8_U2EP1_TX_CTRL &= ~USBHS_UEP_T_DONE;
                bot_in_done();
            } else {
                /* not ours: just acknowledge */
                R8_USB2_INT_FG = USBHS_UDIF_TRANSFER;
            }
        } else {
            /* SETUP or OUT */
            if (ep == 0) {
                if (R8_U2EP0_RX_CTRL & USBHS_UEP_R_SETUP_IS) {
                    handle_setup();
                } else {
                    /* status stage of an IN request */
                    R8_U2EP0_RX_CTRL = USBHS_UEP_R_RES_NAK;
                }
                R8_U2EP0_RX_CTRL &= ~USBHS_UEP_R_DONE;
            } else if (ep == 1) {
                R8_U2EP1_RX_CTRL &= ~USBHS_UEP_R_DONE;
                if (R8_U2EP1_RX_CTRL & USBHS_UEP_R_TOG_MATCH) {
                    uint16_t len = R16_U2EP1_RX_LEN;
                    R8_U2EP1_RX_CTRL ^= USBHS_UEP_R_TOG_DATA1;
                    ep1_out_set(USBHS_UEP_R_RES_NAK);
                    bot_out_packet(ep1_rx_buf, len);
                }
            } else {
                R8_USB2_INT_FG = USBHS_UDIF_TRANSFER;
            }
        }
    } else if (flags & USBHS_UDIF_BUS_RST) {
        dev_addr = 0;
        dev_config = 0;
        R8_USB2_DEV_AD = 0;
        endpoints_init();
        bot_reset();
        R8_USB2_INT_FG = USBHS_UDIF_BUS_RST;
    } else if (flags) {
        R8_USB2_INT_FG = flags;
    }
    return activity;
}

bool usb_msc_idle(void)
{
    return bot_state == BOT_CBW;
}
