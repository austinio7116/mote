/* ThumbyElite — sphere collisions (ship/ship, ship/rock, player/station). */
#ifndef ELITE_COLLIDE_H
#define ELITE_COLLIDE_H

/* Run after movement each flight tick. station_alive: an anchored
 * station sits at the origin with bound radius station_r.
 * player_manual: 1 only in manual flight (autodock exempt). */
void collide_tick(int station_alive, float station_r, int player_manual);

#endif
