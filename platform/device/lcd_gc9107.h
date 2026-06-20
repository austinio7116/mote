/*
 * Mote device — GC9107 LCD driver (ported from ThumbyCraft).
 *
 * 128x128 RGB565 panel, 4-wire SPI on spi0 @ 80 MHz, async DMA push.
 * Pin map (Thumby Color): GP18 SCK, GP19 MOSI, GP17 CS, GP16 DC,
 * GP4 RST, GP7 BL. Standalone only — no ThumbyOne slot/backlight bridge.
 */
#ifndef MOTE_LCD_GC9107_H
#define MOTE_LCD_GC9107_H

#include <stdint.h>

void mote_lcd_init(void);
void mote_lcd_present(const uint16_t *fb_rgb565);   /* wait prior + kick async DMA */
void mote_lcd_kick(const uint16_t *fb_rgb565);      /* kick async DMA, no wait */
void mote_lcd_wait_idle(void);
int  mote_lcd_busy(void);                           /* DMA/SPI still flushing? */
void mote_lcd_backlight(int on);
void mote_lcd_brightness(int pct);   /* 0..100, PWM-dimmed */

#endif
