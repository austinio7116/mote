/*
 * ThumbyElite — controller binding action set (shared game <-> shell).
 *
 * The game UI lists these actions and asks the shell to bind / label them;
 * the shell (host SDL) owns the device-specific axis/button indices. Keeping
 * the enums here means the device stub and every shell agree on the indices.
 */
#ifndef ELITE_CTRL_H
#define ELITE_CTRL_H

/* Flight axes — first CTRL_AX_N entries of a binding table. */
typedef enum {
    CTRL_AX_ROLL = 0,
    CTRL_AX_PITCH,
    CTRL_AX_YAW,
    CTRL_AX_THROTTLE,
    CTRL_AX_N
} CtrlAxis;

/* Button actions. FIRE/CYCLE_WEAPON/MENU map onto raw CraftRawButtons in the
 * shell; FIRE2/FIRE3 are held; the rest inject one-shot events via
 * elite_input_action(). The enum order is the screen's row order. */
typedef enum {
    CTRL_BTN_FIRE = 0,      /* primary fire (active weapon) */
    CTRL_BTN_FIRE2,         /* fire weapon slot 1 */
    CTRL_BTN_FIRE3,         /* fire weapon slot 2 */
    CTRL_BTN_CYCLE_WEAPON,  /* rotate the primary/active weapon */
    CTRL_BTN_CYCLE_TARGET,
    CTRL_BTN_TARGET_MODE,   /* cycle targeting mode (device = hold LB) */
    CTRL_BTN_ASSIST,        /* flight-assist toggle */
    CTRL_BTN_BOOST,
    CTRL_BTN_CHAFF,
    CTRL_BTN_CLOAK,
    CTRL_BTN_DOCK,
    CTRL_BTN_MENU,
    CTRL_BTN_MENU_SELECT,   /* in menus: A / confirm (does nothing in flight) */
    CTRL_BTN_MENU_BACK,     /* in menus: B / cancel  (does nothing in flight) */
    CTRL_BTN_MENU_INFO,     /* in menus: Info / details / alt view */
    CTRL_BTN_N
} CtrlButton;

/* capture kind */
#define CTRL_KIND_AXIS   0
#define CTRL_KIND_BUTTON 1

#endif
