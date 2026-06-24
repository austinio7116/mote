/*
 * ThumbyElite — flight model (Newtonian-lite with flight assist).
 *
 * Throttle sets a target forward speed; with assist ON the velocity
 * vector chases the nose (controllable on a d-pad), with assist OFF
 * thrust only adds along the nose and momentum carries (drift mode).
 */
#ifndef ELITE_FLIGHT_H
#define ELITE_FLIGHT_H

#include "elite_entity.h"
#include "elite_input.h"

/* Apply player controls to g_ships[PLAYER]. */
void flight_apply_input(const FlightInput *in, float dt);

/* Integrate physics for every live ship. */
void flight_tick(float dt);

#endif
