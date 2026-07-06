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

/* While the 2-player link is started it owns the USB controller — this channel
 * must neither pump tud/protocol (it would eat the game's link bytes) nor
 * write log lines into the pipe the peer is reading. */
#if MOTE_LINK_USB
#include "mote_link.h"
#define LINK_OWNS_USB() mote_link_active()
#else
#define LINK_OWNS_USB() 0
#endif

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

/* ---- protocol ------------------------------------------------------------
 * Three build-selected backends share the descriptors/init above:
 *   (default)         store-backed  — standalone Mote OS (mote_store)
 *   MOTE_USB_FAT      FAT-backed    — ThumbyOne lobby: PUT/LIST over /mote/
 *   MOTE_USB_LOGONLY  log channel   — ThumbyOne runner: enumerate + PING + logs
 * MOTE_USB_GATED (the runner) keeps USB off until the engine menu enables it. */

static int s_launch_req = -1;
int mote_usb_take_launch(void) { int r = s_launch_req; s_launch_req = -1; return r; }

static void cdc_say(const char *s) { tud_cdc_write(s, strlen(s)); tud_cdc_write_flush(); }

#if MOTE_USB_GATED
static int s_logs_on = 0, s_usb_inited = 0;
int  mote_usb_logs_enabled(void) { return s_logs_on; }
void mote_usb_logs_set(int on) {
    if (on && !s_usb_inited) { tusb_init(); s_usb_inited = 1; }
    if (on && !s_logs_on) {
        /* Burst-service enumeration so the host handshake completes promptly
         * (the per-frame pump alone is too slow for the initial descriptors). */
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        while (to_ms_since_boot(get_absolute_time()) - t0 < 400) tud_task();
    }
    s_logs_on = on;
}
#endif

/* Stream a log line to the host. Non-blocking: if no host is reading, the FIFO
 * fills and the write is dropped — never stalls a frame. */
void mote_usb_log(const char *s) {
    if (LINK_OWNS_USB()) return;
#if MOTE_USB_GATED
    if (!s_logs_on) return;
#endif
    if (!tud_cdc_connected()) return;
    tud_cdc_write(s, strlen(s));
    tud_cdc_write("\n", 1);
    tud_cdc_write_flush();
}

void mote_usb_init(void) { tusb_init(); }

static void handle_line(const char *cmd);   /* backend-specific (defined below) */

/* Read CDC bytes into a line buffer and dispatch newline-terminated commands. */
static void cdc_pump_lines(void) {
    static char line[128];
    static int  len = 0;
    while (tud_cdc_available()) {
        int c = tud_cdc_read_char();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') { line[len] = 0; handle_line(line); len = 0; }
        else if (len < (int)sizeof(line) - 1) line[len++] = (char)c;
        else len = 0;   /* overlong line: drop */
    }
}

/* ---------- backend: log-only (runner) ----------------------------------- */
#if defined(MOTE_USB_LOGONLY)
static void handle_line(const char *cmd) {
    if (strcmp(cmd, "PING") == 0) { char b[24]; snprintf(b, sizeof b, "MOTE %d\n", MOTE_USB_PROTO); cdc_say(b); }
    /* No game library in the runner — push/list/launch are lobby-only. */
}
void mote_usb_task(void) {
    if (LINK_OWNS_USB()) return;     /* the 2P link owns the controller */
#if MOTE_USB_GATED
    if (!s_logs_on) return;          /* off → zero USB work during play */
#endif
    tud_task();
    cdc_pump_lines();
}

/* ---------- backend: FAT-backed /mote/ (lobby) --------------------------- */
#elif defined(MOTE_USB_FAT)
#include "ff.h"
#include "mote_module.h"    /* MoteModuleHeader / MOTE_MODULE_VADDR — read a .mote's embedded version */
#define MOTE_DIR "/mote"
static FIL      s_putf; static int s_put_open, s_rx_active;
static uint32_t s_put_size, s_put_got;
/* Read a .mote's embedded game version (ABI v46+) + its ABI straight from the FAT
 * file header, so the gallery can diff installed vs available. version defaults to
 * "0" (unversioned) for older modules that predate the version field. */
static void mote_read_meta(const char *path, char *ver, int vn, int *abi) {
    ver[0] = '0'; ver[1] = 0; if (abi) *abi = 0;
    FIL f; if (f_open(&f, path, FA_READ) != FR_OK) return;
    MoteModuleHeader h; UINT br = 0;
    if (f_read(&f, &h, sizeof h, &br) == FR_OK && br == sizeof h && h.magic == MOTE_MODULE_MAGIC) {
        if (abi) *abi = (int)h.abi_version;
        if (h.abi_version >= 46 && h.version_vaddr &&
            f_lseek(&f, h.version_vaddr - MOTE_MODULE_VADDR) == FR_OK) {
            char tmp[16]; UINT b2 = 0;
            if (f_read(&f, tmp, sizeof tmp - 1, &b2) == FR_OK && b2 > 0) {
                tmp[b2] = 0; int i = 0;
                for (; i < vn - 1 && tmp[i] > ' '; i++) ver[i] = tmp[i];
                ver[i] = 0; if (!ver[0]) { ver[0] = '0'; ver[1] = 0; }
            }
        }
    }
    f_close(&f);
}
static void handle_line(const char *cmd) {
    char name[40]; unsigned size;
    if (strcmp(cmd, "PING") == 0) {
        char b[40]; snprintf(b, sizeof b, "MOTE %d ABI %d\n", MOTE_USB_PROTO, MOTE_ABI_VERSION); cdc_say(b);
    } else if (strcmp(cmd, "LIST") == 0) {
        DIR d; FILINFO fi; int i = 0;
        if (f_opendir(&d, MOTE_DIR) == FR_OK) {
            while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
                if (fi.fattrib & AM_DIR) continue;
                char path[96]; snprintf(path, sizeof path, "%s/%s", MOTE_DIR, fi.fname);
                char ver[16]; int abi; mote_read_meta(path, ver, sizeof ver, &abi);
                char b[112]; snprintf(b, sizeof b, "%d %s %s %d\n", i++, fi.fname, ver, abi); cdc_say(b);
            }
            f_closedir(&d);
        }
        cdc_say("OK\n");
    } else if (sscanf(cmd, "PUT %39s %u", name, &size) == 2) {
        /* the IDE sends the bare game name (no extension) — land it as <name>.mote
         * so the launcher lists it and the runner can map it. */
        size_t ln = strlen(name);
        int has_ext = ln > 5 && strcmp(name + ln - 5, ".mote") == 0;
        char path[80]; snprintf(path, sizeof path, "%s/%s%s", MOTE_DIR, name, has_ext ? "" : ".mote");
        f_mkdir(MOTE_DIR);   /* ok if it exists */
        if (f_open(&s_putf, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { cdc_say("ERR open\n"); return; }
        /* 4 KB clusters (FAT12) → the file is born 4 KB-aligned for the runner's
         * ATRANS XIP map; no special placement needed. */
        s_put_open = 1; s_put_size = size; s_put_got = 0; s_rx_active = 1;
        cdc_say("READY\n");
    } else if (cmd[0]) {
        cdc_say("ERR unknown\n");
    }
}
void mote_usb_task(void) {
    if (LINK_OWNS_USB()) return;     /* the 2P link owns the controller */
    tud_task();
    if (s_rx_active) {
        while (s_put_got < s_put_size && tud_cdc_available()) {
            uint8_t tmp[64]; UINT bw = 0;
            uint32_t want = s_put_size - s_put_got; if (want > sizeof tmp) want = sizeof tmp;
            uint32_t n = tud_cdc_read(tmp, want);
            if (n) { f_write(&s_putf, tmp, n, &bw); s_put_got += n; }
        }
        if (s_put_got >= s_put_size) {
            s_rx_active = 0;
            if (s_put_open) { f_close(&s_putf); s_put_open = 0; }
            cdc_say("OK\n");
        }
        return;
    }
    cdc_pump_lines();
}

/* ---------- backend: store-backed (standalone) — original behavior ------- */
#else
#include "mote_store.h"
static int  s_rx_active;
static uint32_t s_put_size, s_put_got;
static void handle_line(const char *cmd) {
    char name[MOTE_STORE_NAME_MAX]; unsigned size;
    if (strcmp(cmd, "PING") == 0) {
        char b[24]; snprintf(b, sizeof b, "MOTE %d\n", MOTE_USB_PROTO); cdc_say(b);
    } else if (strcmp(cmd, "LIST") == 0) {
        char b[48];
        for (int i = 0; i < mote_store_count(); i++) {
            snprintf(b, sizeof b, "%d %s\n", i, mote_store_get(i)->name); cdc_say(b);
        }
        cdc_say("OK\n");
    } else if (sscanf(cmd, "PUT %19s %u", name, &size) == 2) {
        if (mote_store_begin(name, size) != 0) { cdc_say("ERR size\n"); return; }
        s_put_size = size; s_put_got = 0; s_rx_active = 1;
        cdc_say("READY\n");
    } else if (sscanf(cmd, "LAUNCH %19s", name) == 1) {
        int idx = mote_store_find(name);
        if (idx >= 0) { s_launch_req = idx; cdc_say("OK\n"); } else cdc_say("ERR notfound\n");
    } else if (strcmp(cmd, "WIPE") == 0) {
        mote_store_wipe(); cdc_say("OK\n");
    } else if (cmd[0]) {
        cdc_say("ERR unknown\n");
    }
}
void mote_usb_task(void) {
    if (LINK_OWNS_USB()) return;     /* the 2P link owns the controller */
    tud_task();
    if (s_rx_active) {
        while (s_put_got < s_put_size && tud_cdc_available()) {
            uint8_t tmp[64];
            uint32_t want = s_put_size - s_put_got; if (want > sizeof tmp) want = sizeof tmp;
            uint32_t n = tud_cdc_read(tmp, want);
            mote_store_write(tmp, n); s_put_got += n;
        }
        if (s_put_got >= s_put_size) {
            s_rx_active = 0;
            int r = mote_store_end();
            cdc_say(r == 0 ? "OK\n" : "ERR write\n");
        }
        return;
    }
    cdc_pump_lines();
}
#endif
