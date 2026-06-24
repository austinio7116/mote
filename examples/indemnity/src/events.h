/*
 * ThumbyElite — random events: one tiny op-list interpreter, many const
 * data rows (events_data.c). Adding content costs flash data bytes only,
 * never code. NPC names + faces derive from the pick seed (zero assets).
 *
 * Selection is seeded per dock visit (system seed ^ visit salt), so a
 * given arrival always offers the same event — no save-scumming — but
 * the next visit rerolls. One-shot events and lore reveals persist in a
 * 256-bit field carried by the save.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include "galaxy_gen.h"
#include <stdint.h>
#include <stdbool.h>

/* --- outcome bytecode -------------------------------------------------- */
enum {
    OP_END = 0,
    OP_CR,        /* credits += a*25                                       */
    OP_CARGO,     /* good a (-1 = seeded trade good) count += b (clamped)  */
    OP_REP,       /* faction a (-1 = local) rep += b                       */
    OP_FUEL,      /* fuel += a tenths of LY (clamped 0..max)               */
    OP_DMG,       /* hull -= a percent of max (never lethal)               */
    OP_AMBUSH,    /* a pirates of tier b spawned outside — wait for launch */
    OP_LORE,      /* reveal lore fragment a (sets bit, result shows text)  */
    OP_FLAG,      /* set story flag a (gate later events on it)            */
    OP_BRANCH,    /* a% chance: jump to op index b, else fall through      */
    OP_RESULT,    /* aftermath text = event->texts[a] (last one wins)      */
    OP_LEGAL,     /* legal status += a (0 clean / 1 offender / 2 fugitive) */
    OP_CONTRA,    /* confiscate all illegal cargo                          */
    OP_ITEM,      /* salvaged hardware into the rack (a = quality floor;
                     rack full -> 100 CR scrap value instead)              */
    OP_LATER,     /* a*25 CR arrives at the NEXT dock (deferred transfer)  */
    OP_MISSION,   /* log a contract (a: 0 = warzone). No-op if log full    */
    OP_TIER,      /* a=0 shield / 1 armour: fitted tier += b (clamp 1..3)  */
    OP_AFFIX,     /* set affix a on the primary fitted weapon (Affix enum) */
};

typedef struct { uint8_t op; int8_t a; int8_t b; } Op;

/* --- gates (event must pass all set bits to be offered; a CHOICE that
 *     fails its gate shows greyed — the FTL "blue option" inverted) ----- */
#define GATE_LAWFUL      0x0001   /* gov >= CONFED                  */
#define GATE_ROUGH       0x0002   /* anarchy/feudal                 */
#define GATE_THREAT      0x0004   /* threat >= 2                    */
#define GATE_ILLEGAL     0x0008   /* carrying illegal cargo         */
#define GATE_CARGO_SPACE 0x0010   /* >= 2 free cargo                */
#define GATE_FUEL_SPARE  0x0020   /* fuel >= 2.0 ly                 */
#define GATE_CLEAN       0x0040   /* legal == CLEAN                 */
#define GATE_WANTED      0x0080   /* legal > CLEAN                  */
#define GATE_HAS_MEDS    0x0100   /* carrying MEDICINE              */
#define GATE_NO_ILLEGAL  0x0200   /* hold is clean of contraband    */
#define GATE_FRONTLINE   0x0400   /* near a faction front           */
#define GATE_REP_PLUS    0x0800   /* local faction rep >= 2         */

typedef struct {
    const char *label;        /* "GIVE THEM FUEL"                          */
    uint16_t    gate;         /* bits required to enable (0 = always)      */
    int16_t     cost;         /* credits required AND deducted (0 = free)  */
    const Op   *ops;          /* OP_END-terminated                         */
} Choice;

#define EV_ONESHOT 0x01       /* never offered again once seen            */

/* Where an event can fire. */
enum { TRIG_DOCK = 0, TRIG_BAR, TRIG_SPACE, TRIG_ARRIVAL };

/* NPC portrait archetype hint (biases r3d_face). */
enum { NK_CIVILIAN = 0, NK_OFFICIAL, NK_PIRATE, NK_MYSTIC, NK_DOCKHAND,
       NK_NONE = 0xFF };

typedef struct Event {
    uint8_t  id;              /* stable, unique — seen/suppression key     */
    uint8_t  weight;
    uint8_t  flags;           /* EV_*                                      */
    uint8_t  npc_kind;        /* NK_*                                      */
    uint8_t  trig;            /* TRIG_*                                    */
    uint8_t  need_flag;       /* 0 none, else story flag (id+1) must be SET
                                 — chains authored arcs in order           */
    uint8_t  not_flag;        /* 0 none, else flag (id+1) must be CLEAR    */
    uint8_t  fixed_npc;       /* 0 = per-pick face/name; else a stable
                                 campaign identity (recurring character)   */
    uint16_t gate;            /* bits required to offer at all             */
    const char *title;
    const char *body;         /* tokens: $N name $S system $T station
                                         $F faction $G trade good          */
    const char *const *texts; /* OP_RESULT strings (token-expanded too)    */
    const Choice *choices;
    uint8_t n_choices;
} Event;

typedef struct { const char *title, *body; } Lore;

/* --- pool (events_data.c) ---------------------------------------------- */
extern const Event k_events[];
extern const int   k_n_events;
extern const Lore  k_lore[];
extern const int   k_n_lore;

/* --- engine ------------------------------------------------------------ */
void events_init(void);                       /* new game: clear all bits */

/* Roll the dock-arrival hail. NULL = quiet arrival (most docks). */
const Event *events_roll_dock(const SystemInfo *si, int station);
/* Roll the bar encounter (once per dock visit; ui_station owns it). */
const Event *events_roll_bar(const SystemInfo *si, int station);
/* Boarding a derelict — the spawn was the odds, so this always deals
 * if the pool has an eligible event (NULL = hulk already stripped). */
const Event *events_roll_space(const SystemInfo *si);
/* Supercruise-drop comm hail (rare — docks carry the hail load). */
const Event *events_roll_arrival(const SystemInfo *si);

bool events_choice_enabled(const Event *ev, int choice);
/* Deduct cost, run ops. Returns texts[] index for the aftermath panel,
 * or -1 (generic). Outcome is deterministic per visit (branch rng is
 * seeded by the pick) — choosing, reloading and rechoosing can't reroll. */
int events_run_choice(const Event *ev, int choice);

/* What the last choice actually changed — the aftermath panel prints
 * these so no outcome is ever invisible (user req). */
typedef struct {
    int32_t cr;               /* credits delta (cost included)         */
    int32_t later_cr;         /* arrives at next dock                  */
    float   fuel;             /* LY delta                              */
    int     hull_pct;         /* hull delta, % of max                  */
    int8_t  rep[3];           /* per-faction rep delta                 */
    int     legal;            /* legal-status delta                    */
    int8_t  goods_d[3];       /* up to 3 changed goods...              */
    uint8_t goods_id[3];
    uint8_t n_goods;
    int     lore_id;          /* revealed fragment, -1 none            */
    int     item_type;        /* salvaged hardware type, -1 none       */
    uint8_t ambush_n;         /* hostiles now inbound                  */
    uint8_t mission;          /* contract logged this choice           */
} EvReceipt;
const EvReceipt *events_receipt(void);

/* Deferred transfers (OP_LATER): the dock pays them out. */
int32_t events_pending_take(void);            /* returns + clears      */
int32_t *events_save_pending(void);           /* save bridge           */

/* Expand $-tokens of this event's pick (NPC name etc.) into out. */
void events_expand(const char *tmpl, char *out, int cap);
uint32_t events_npc_seed(void);               /* current pick's NPC        */

bool events_lore_seen(int id);
bool events_flag(int id);
void events_set_flag(int id);   /* cheat / climax arming */
void events_set_lore(int id);   /* cheat: mark a fragment pre-seen */
const Event *events_get(int id);/* fetch a specific event (scripted opens) */

/* Save bridge: 32B bit-field (lore 0..127, flags 128..159, seen 160..255)
 * + 8-id recent ring. Pointers into live state — memcpy in/out. */
uint8_t *events_save_bits(void);              /* EVENTS_BITS_LEN          */
uint8_t *events_save_recent(void);            /* EVENTS_RECENT_LEN        */
#define EVENTS_BITS_LEN   32
#define EVENTS_RECENT_LEN 8

/* Tests: pin the visit salt for deterministic asserts. */
void events_set_salt(uint32_t salt);
/* Tests/harnesses: override the hail chance (0 disables, -1 restores
 * the default). Scripted button-drivers must not hit surprise modals. */
void events_set_chance(int pct);

#endif
