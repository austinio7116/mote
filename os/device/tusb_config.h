/*
 * Mote OS — TinyUSB configuration.
 *
 * CDC (serial) device: Mote is a development platform, so USB is the
 * host<->device control + debug channel (push modules, launch, stream logs/
 * profiler, IDE integration) — not a consumer file-copy drive. CDC is the
 * right class for that bidirectional, scriptable link.
 *
 * Settings mirror ThumbyOne's PROVEN RP2350 TinyUSB setup (same SDK, same
 * board) — notably OPT_MCU_RP2040 and the mandatory RHPORT0_MODE — but with
 * the CDC class enabled instead of MSC.
 */
#ifndef MOTE_TUSB_CONFIG_H
#define MOTE_TUSB_CONFIG_H

#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUSB_DEBUG          0

/* CRITICAL: without CFG_TUSB_RHPORT0_MODE, tusb_init() succeeds but never
 * calls tud_init() — the device silently doesn't enumerate. */
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

/* Class enables — CDC only. */
#define CFG_TUD_CDC             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

/* CDC FIFO + endpoint buffer sizes. */
#define CFG_TUD_CDC_RX_BUFSIZE  256
#define CFG_TUD_CDC_TX_BUFSIZE  256
#define CFG_TUD_CDC_EP_BUFSIZE  64

#endif /* MOTE_TUSB_CONFIG_H */
