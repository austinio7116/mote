/*
 * ThumbyElite — event modal (dock hails). Caller opens with a picked
 * event, ticks until true (closed), then resumes the dock flow.
 */
#ifndef UI_EVENT_H
#define UI_EVENT_H

#include "events.h"
#include "craft_buttons.h"
#include <stdint.h>
#include <stdbool.h>

void ui_event_open(const Event *ev);
bool ui_event_tick(const CraftRawButtons *btn, float dt);
void ui_event_draw(uint16_t *fb);

#endif
