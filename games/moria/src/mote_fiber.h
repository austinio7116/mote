/* games/moria/src/mote_fiber.h: fiber interface

   Copyright (C) 2026 austinio7116

   This file is part of the Mote port of Umoria to the Thumby Color, and is
   distributed as part of that combined work.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>. */

/* mote_fiber.h : cooperative fiber that lets Umoria's blocking main loop run
   underneath the Mote engine's per-frame update() callback.

   Umoria is structured as main() -> ... -> inkey()/getch(), a loop that blocks
   waiting for a keystroke.  The Mote engine instead calls update(dt) once per
   frame and expects it to return promptly.  We reconcile the two by running the
   whole Umoria game on its own stack (a "fiber"):

     - fiber_resume() switches from the engine into the game and returns when
       the game next yields (it is waiting for input, or voluntarily giving the
       engine a frame to render).
     - fiber_yield() is called from deep inside the game (getch / check_input)
       to hand a frame back to the engine.

   Host build uses ucontext.  (A device build needs a small custom context
   switch; deferred -- see mote_fiber.c.) */

#ifndef MOTE_FIBER_H
#define MOTE_FIBER_H

/* Install the game entry point; does not run it yet. */
void fiber_start(void (*entry)(void));

/* Engine side: run the game until it yields (or finishes). */
void fiber_resume(void);

/* Game side: give a frame back to the engine.  Returns when resumed. */
void fiber_yield(void);

/* Non-zero once the game entry has returned / exit_game() was reached. */
int fiber_finished(void);

/* Game side: abandon the game and return to the engine for good (used by
   exit_game()).  Never returns. */
void fiber_finish(void);

#endif /* MOTE_FIBER_H */
