/*
 * Mote OS device — flash/XIP maintenance for runtime flash writes.
 *
 * RP2350 flash erase/program (via the bootrom) leaves QMI in slow cmd-XIP mode
 * and can reshape ATRANS. After any flash op we must restore fast QPI XIP and
 * the ATRANS windows, or the whole OS runs ~2x slower and game-window mapping
 * breaks. (Same recipe ThumbyOne uses around its FAT writes.)
 */
#ifndef MOTE_XIP_H
#define MOTE_XIP_H

#include <stdint.h>

void mote_xip_save_atrans(uint32_t saved[4]);
void mote_xip_restore_atrans(const uint32_t saved[4]);
void mote_xip_fast_setup(void);   /* re-establish fast QPI XIP (runs from RAM) */

#endif /* MOTE_XIP_H */
