/*
 * Mote OS — USB-CDC device + line protocol.
 *
 * Descriptor/init structure mirrors ThumbyOne's proven RP2350 TinyUSB code,
 * adapted MSC->CDC. Protocol is newline-terminated ASCII commands; replies
 * are newline-terminated, ending a multi-line reply with "OK"/"ERR".
 *
 *   PING            -> "MOTE <proto>\n"          (handshake)
 *   LIST            -> "<idx> <name>\n" * N, "OK\n"
 *   (2c-2: PUT <name> <size> <crc> + data; LAUNCH <idx>; LOG stream)
 */
#include "mote_usb.h"

#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>
#include <stdio.h>

#define MOTE_USB_PROTO 1

/* ---- descriptors ---------------------------------------------------- */

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* CDC is a composite (IAD) device. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0xCAFE,
    .idProduct          = 0x4D01,   /* 'M01' — Mote, distinct from ThumbyOne */
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF  0x81
#define EPNUM_CDC_OUT    0x02
#define EPNUM_CDC_IN     0x82

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_fs_configuration;
}

static char const *const string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: language id — English */
    "Mote",                        /* 1: manufacturer */
    "Mote Device",                 /* 2: product */
    NULL,                          /* 3: serial — filled at runtime */
    "Mote CDC",                    /* 4: CDC interface */
};

static uint16_t _desc_str[32];
static char _serial[5 + 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1] = {0};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;
    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else if (index == 3) {
        if (_serial[0] == 0) {
            pico_unique_board_id_t bid;
            pico_get_unique_board_id(&bid);
            static const char hex[] = "0123456789ABCDEF";
            memcpy(_serial, "MOTE-", 5);
            for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
                _serial[5 + i * 2 + 0] = hex[(bid.id[i] >> 4) & 0xF];
                _serial[5 + i * 2 + 1] = hex[bid.id[i] & 0xF];
            }
            _serial[5 + 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES] = 0;
        }
        chr_count = (uint8_t)strlen(_serial);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = _serial[i];
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *s = string_desc_arr[index];
        if (!s) return NULL;
        chr_count = (uint8_t)strlen(s);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = s[i];
    }
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* ---- protocol ------------------------------------------------------- */

static const MoteCatalog *s_cat;

void mote_usb_set_catalog(const MoteCatalog *cat) { s_cat = cat; }

static void cdc_say(const char *s) {
    tud_cdc_write(s, strlen(s));
    tud_cdc_write_flush();
}

static void handle(const char *cmd) {
    if (strcmp(cmd, "PING") == 0) {
        char b[24];
        snprintf(b, sizeof b, "MOTE %d\n", MOTE_USB_PROTO);
        cdc_say(b);
    } else if (strcmp(cmd, "LIST") == 0) {
        if (s_cat) {
            char b[48];
            for (int i = 0; i < s_cat->count; i++) {
                snprintf(b, sizeof b, "%d %s\n", i, s_cat->e[i].name);
                cdc_say(b);
            }
        }
        cdc_say("OK\n");
    } else if (cmd[0] == 0) {
        /* ignore blank lines */
    } else {
        cdc_say("ERR unknown\n");
    }
}

void mote_usb_init(void) { tusb_init(); }

void mote_usb_task(void) {
    tud_task();
    static char line[128];
    static int  len = 0;
    while (tud_cdc_available()) {
        int c = tud_cdc_read_char();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') { line[len] = 0; handle(line); len = 0; }
        else if (len < (int)sizeof(line) - 1) line[len++] = (char)c;
        else len = 0;   /* overlong line: drop */
    }
}
