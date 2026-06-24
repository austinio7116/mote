/*
 * ThumbyElite — docked station services (market, refuel, launch).
 */
#ifndef UI_STATION_H
#define UI_STATION_H

#include "craft_buttons.h"
#include <stdint.h>

typedef enum { DOCK_NONE = 0, DOCK_LAUNCH, DOCK_EVENT } DockAction;

void station_open(int station_idx);
void station_toast(const char *msg);
/* DOCK_EVENT handshake: the bar encounter to open (consumed). */
const struct Event *station_pending_event(void);
/* 3D preview pane: returns 0 none, 1 station, 2 ship (seed+class out). */
int station_preview2(uint32_t *mesh_seed, int *class_hint);
DockAction station_tick(const CraftRawButtons *btn, float dt);
int station_debug_screen(void);   /* guide harness: current SCR_* as int */
void station_draw(uint16_t *fb);

#endif
