/*
 * ThumbyElite — detail sheets (drill-down stat views).
 *
 * Shared by the shipyard (hull specs), outfitting (weapon specs before
 * buying/fitting) and the status screen (mounted/racked gear + cargo).
 * Pure draw functions; callers own the open/close input flow.
 */
#ifndef UI_DETAIL_H
#define UI_DETAIL_H

#include "elite_player.h"
#include <stdint.h>

/* Weapon sheet: effective stats for THIS instance (quality+integrity).
 * price >= 0 draws a price line with the given label ("COST"/"REPAIR"/
 * "SELL"). footer = button hints. */
/* cmp: currently-fitted item to diff against (NULL = no comparison).
 * Deltas render beside each stat, green = better, red = worse. */
void detail_draw_weapon(uint16_t *fb, const WeaponInst *wi,
                        const WeaponInst *cmp,
                        int price, const char *price_label,
                        const char *footer);

/* Hull sheet: full specs. cost == DETAIL_OWNED renders OWNED; a negative cost
 * is a trade-down refund (renders GET). The caller's 3D pane (right column,
 * y 10..95) stays open — text fits the left column. */
#define DETAIL_OWNED (-2000000000)
void detail_draw_hull(uint16_t *fb, int hull_id, uint32_t seed, int cost,
                      const char *footer);
void detail_hull_scroll(int d);       /* up/down scroll of the hull spec column */
void detail_hull_scroll_reset(void);

/* Commodity sheet (status cargo drill-down). */
void detail_draw_good(uint16_t *fb, int good, int held,
                      const char *footer);

#endif
