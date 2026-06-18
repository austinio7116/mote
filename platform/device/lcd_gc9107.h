/*
 * ThumbyEngine device — GC9107 LCD driver (ported from ThumbyCraft).
 *
 * 128x128 RGB565 panel, 4-wire SPI on spi0 @ 80 MHz, async DMA push.
 * Pin map (Thumby Color): GP18 SCK, GP19 MOSI, GP17 CS, GP16 DC,
 * GP4 RST, GP7 BL. Standalone only — no ThumbyOne slot/backlight bridge.
 */
#ifndef TE_LCD_GC9107_H
#define TE_LCD_GC9107_H

#include <stdint.h>

void te_lcd_init(void);
void te_lcd_present(const uint16_t *fb_rgb565);   /* starts async DMA */
void te_lcd_wait_idle(void);
void te_lcd_backlight(int on);

#endif
