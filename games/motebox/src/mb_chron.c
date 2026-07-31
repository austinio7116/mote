/*
 * Motebox — the chronicle: names, events, legends.
 *
 * The cheapest big win in the design (DESIGN.md 9). A ring of 96 events at 12
 * bytes each, rendered to text only when something displays it, from templates
 * plus a syllable name generator. Nothing stores a string: a name is a 16-bit id
 * and the generator is a pure function of it, so every name in the world costs
 * two bytes and is stable for as long as the id is.
 *
 * It also feeds the simulation back: mb_chron_grudge() is a query over this ring,
 * which is why a king remembers who sacked what without a diplomacy table.
 */
#include "mb.h"
#include <stdio.h>
#include <string.h>

/* --- names -------------------------------------------------------------- *
 * Three syllable tables, chosen so the three kinds of name do not sound alike:
 * places are blunt and Anglo, kingdoms are grander, people are softer. */
static const char *const P1[] = { "Ember", "Stone", "Grim", "Ash", "Ford", "Oak",
    "Bright", "Black", "Hollow", "Thorn", "Frost", "Glen", "Raven", "Dun",
    "Silver", "Storm" };
static const char *const P2[] = { "hold", "watch", "ford", "vale", "reach", "gate",
    "mere", "fell", "burn", "wick", "moor", "haven", "crest", "barrow", "stead", "gard" };

static const char *const K1[] = { "Aeth", "Vor", "Kal", "Mor", "Tess", "Ur",
    "Zan", "Hal", "Ery", "Bel", "Cor", "Dral", "Ith", "Ny", "Oss", "Rhun" };
static const char *const K2[] = { "ia", "mark", "gard", "heim", "or", "esh",
    "and", "ith", "ora", "un", "arn", "elle", "oth", "ys", "adre", "en" };

static const char *const N1[] = { "Kae", "Bren", "Sil", "Tor", "Mae", "Dov",
    "Ash", "Rel", "Ny", "Gar", "Ith", "Vel", "Ora", "Fen", "Lys", "Cade" };
static const char *const N2[] = { "da", "wyn", "ric", "th", "la", "gan", "ra",
    "vek", "iel", "mund", "sa", "dor", "en", "wick", "ys", "ne" };

static uint32_t nh(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

uint16_t mb_name_place(uint32_t salt)   { return (uint16_t)(nh(salt ^ 0xA1u) & 0xFFFF); }
uint16_t mb_name_kingdom(uint32_t salt) { return (uint16_t)(nh(salt ^ 0xB2u) & 0xFFFF); }
uint16_t mb_name_person(uint32_t salt)  { return (uint16_t)(nh(salt ^ 0xC3u) & 0xFFFF); }


void mb_name_str(char *out, int n, int kind, uint16_t id)
{
    const char *const *a, *const *b;
    switch (kind) {
    case NK_KINGDOM: a = K1; b = K2; break;
    case NK_PERSON:  a = N1; b = N2; break;
    default:         a = P1; b = P2; break;
    }
    snprintf(out, (size_t)n, "%s%s", a[id & 15], b[(id >> 4) & 15]);
}

/* --- the ring ----------------------------------------------------------- */

/* EV_TECH AND EV_LORD EXIST BECAUSE THEY WERE BEING FAKED. A kingdom learning something was
 * pushed as a DISASTER carrying the tech's name, so the log line for the discovery of tools
 * was the single word "tools" — no subject, no verb, and sitting in the same list as "a
 * plague". A new lord was pushed as an AGE carrying the town's TIER, so the death of a lord
 * read "the hamlet begins". Both are proper events with proper sentences now. */
enum { EV_FOUND = 0, EV_FALL, EV_BUILD, EV_WAR, EV_PEACE, EV_REBEL, EV_BIRTH,
       EV_DEATH, EV_LEGEND, EV_DISASTER, EV_AGE, EV_TECH, EV_LORD, EV_TAKEN, EV_ALLY, EV_N };

typedef struct {
    uint8_t  type, a, b, mag;      /* a/b: village, kingdom or unit, per type */
    uint16_t year;
    uint16_t name;                 /* the id the line needs to render */
    uint8_t  x, y;                 /* where, for the camera to follow */
    uint16_t extra;
} Event;

/* 160, UP FROM 96. Now that a lord is a real person who dies, a world of ten towns produces
 * about one succession a year — and a 96-entry ring covers less than a century of that, so the
 * wars, conquests and rebellions a player actually wants to read were being pushed off the end
 * by "Garvek rules Oakwick". Sixty-four more entries is 768 bytes and about two more centuries
 * of history. */
#define NEV 160
static Event s_ev[NEV];
static int   s_n, s_head;
static char  s_toast[44];
static float s_toast_t;
static uint8_t s_focus_x, s_focus_y, s_have_focus;

/* Only these are worth interrupting the player for. A birth is a line in the log;
 * a kingdom falling is a headline. */
static int is_headline(int type)
{
    return type == EV_FALL || type == EV_WAR || type == EV_REBEL ||
           type == EV_LEGEND || type == EV_DISASTER || type == EV_AGE ||
           type == EV_TAKEN || type == EV_ALLY;
}

void mb_chron_init(void)
{
    memset(s_ev, 0, sizeof s_ev);
    s_n = s_head = 0;
    s_toast[0] = 0; s_toast_t = 0.0f; s_have_focus = 0;
}

static void render(char *out, int n, const Event *e);

/* `extra` is a parameter, NOT something the caller pokes in afterwards. It used to
 * be assigned by each caller on the returned Event — but push() renders the toast
 * before returning, so a headline showed whatever the PREVIOUS event in that ring
 * slot had left there. An age change announced itself as "the camp begins". */
static Event *push(int type, int a, int b, int mag, uint16_t name, int x, int y,
                   uint16_t extra)
{
    Event *e = &s_ev[s_head];
    s_head = (s_head + 1) % NEV;
    if (s_n < NEV) s_n++;
    e->type = (uint8_t)type; e->a = (uint8_t)a; e->b = (uint8_t)b;
    e->mag = (uint8_t)mag; e->name = name; e->extra = extra;
    e->year = (uint16_t)(mb_w.tick / 52);
    e->x = (uint8_t)x; e->y = (uint8_t)y;
    if (is_headline(type)) {
        render(s_toast, sizeof s_toast, e);
        s_toast_t = 3.4f;
        /* the chronicle is the right place to hang sound off: a thing worth a
         * headline is exactly a thing worth a noise, so the two can never drift */
        switch (type) {
        case EV_WAR:   mb_snd(SND_WAR);   break;
        case EV_FALL:  mb_snd(SND_FALL);  break;
        case EV_REBEL: mb_snd(SND_WAR);   break;
        case EV_AGE:   mb_snd(SND_AGE);   break;
        default:       break;
        }
        /* only if it HAPPENED somewhere: see mb_chron_age() */
        if (x || y) { s_focus_x = (uint8_t)x; s_focus_y = (uint8_t)y; s_have_focus = 1; }
    }
    return e;
}

/* --- the templates ------------------------------------------------------ */

/* TWENTY-ONE CHARACTERS is the whole line: rogue8 averages 6 px a glyph and the
 * HUD is 128 px wide. The first drafts read "Uror declares war on Belith" and the
 * player saw "Uror declares w" — a headline that cuts off mid-word is worse than a
 * blunt one, so the templates below are written to the width they have. */
static void render(char *out, int n, const Event *e)
{
    char n1[24], n2[24];
    switch (e->type) {
    case EV_FOUND:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        snprintf(out, (size_t)n, "%s is founded", n1);
        break;
    case EV_FALL:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        snprintf(out, (size_t)n, "%s falls", n1);
        break;
    case EV_BUILD:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        snprintf(out, (size_t)n, "%s: a %s", n1, mb_chron_word(e->extra));
        break;
    case EV_WAR:
        mb_name_str(n1, sizeof n1, NK_KINGDOM, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s v %s", n1, n2);      /* two names, 21 chars */
        break;
    case EV_PEACE:
        mb_name_str(n1, sizeof n1, NK_KINGDOM, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s-%s: peace", n1, n2);
        break;
    case EV_REBEL:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s rebels", n1);
        break;
    case EV_BIRTH:
        mb_name_str(n1, sizeof n1, NK_PERSON, e->name);
        snprintf(out, (size_t)n, "%s is born", n1);
        break;
    case EV_DEATH: {
        static const char *const HOW[CAUSE_N] = { "dies old", "dies of wounds",
            "is eaten", "is slain", "is lost", "dies of plague", "starves" };
        mb_name_str(n1, sizeof n1, NK_PERSON, e->name);
        snprintf(out, (size_t)n, "%s %s", n1, HOW[e->mag < CAUSE_N ? e->mag : 0]);
        break;
    }
    case EV_LEGEND: {
        static const char *const WHY[LEGEND_N] = { "the Bloodied", "the Long-Reigning",
            "the Unburnt", "the Founder" };
        mb_name_str(n1, sizeof n1, NK_PERSON, e->name);
        snprintf(out, (size_t)n, "%s %s", n1, WHY[e->mag < LEGEND_N ? e->mag : 0]);
        break;
    }
    case EV_DISASTER:
        snprintf(out, (size_t)n, "%s", mb_chron_word(e->extra));
        break;
    case EV_AGE:
        snprintf(out, (size_t)n, "the %s begins", mb_chron_word(e->extra));
        break;
    case EV_TECH:
        mb_name_str(n1, sizeof n1, NK_KINGDOM, e->name);
        snprintf(out, (size_t)n, "%s learns %s", n1, mb_chron_word(e->extra));
        break;
    case EV_ALLY:
        mb_name_str(n1, sizeof n1, NK_KINGDOM, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s+%s: allied", n1, n2);
        break;
    case EV_TAKEN:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s taken by %s", n1, n2);
        break;
    case EV_LORD:
        /* the lord first: it is the person who is new, and the place is the qualifier */
        mb_name_str(n1, sizeof n1, NK_PERSON, e->name);
        mb_name_str(n2, sizeof n2, NK_PLACE, e->extra);
        snprintf(out, (size_t)n, "%s rules %s", n1, n2);
        break;
    default:
        snprintf(out, (size_t)n, "?");
        break;
    }
}

/* A tiny interned-word table, so an event can carry "barracks" or "Age of Ash"
 * in two bytes without the ring holding char pointers into who-knows-what. */
#define NWORD 48
static const char *s_word[NWORD];
static int s_nword;

const char *mb_chron_word(int id)
{
    return (id >= 0 && id < s_nword && s_word[id]) ? s_word[id] : "?";
}

static uint16_t intern(const char *w)
{
    for (int i = 0; i < s_nword; i++) if (s_word[i] == w) return (uint16_t)i;
    for (int i = 0; i < s_nword; i++)
        if (s_word[i] && !strcmp(s_word[i], w)) return (uint16_t)i;
    if (s_nword >= NWORD) return 0;
    s_word[s_nword] = w;              /* all callers pass string literals */
    return (uint16_t)s_nword++;
}

/* --- the event calls ---------------------------------------------------- */

void mb_chron_found(int v)
{
    push(EV_FOUND, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y, 0);
    mb_snd(SND_FOUND);
}
void mb_chron_fall(int v)
{
    push(EV_FALL, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y, 0);
}
void mb_chron_build(int v, const char *what)
{
    /* A HOUSE IS NOT NEWS EITHER. Houses, farms and woodcutters' camps are what a town does
     * continuously — three of the top four lines in the log were "a house" — so they are the
     * same weather as a birth. Everything else a lord raises is a decision worth recording:
     * a hall, a temple, a market, a college, a silo, a monument. */
    if (what && (!strcmp(what, "house") || !strcmp(what, "camp")
                 || !strcmp(what, "farm"))) return;
    push(EV_BUILD, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y, intern(what));
    mb_snd(SND_BUILD);
}
/* A DISCOVERY IS THE KINGDOM'S, and it is announced from its capital so the camera has
 * somewhere to go. The tech name is interned like any other word. */
void mb_chron_tech(int k, const char *what, int x, int y)
{
    push(EV_TECH, k, 0, 0, mb_k[k].name, x, y, intern(what));
}
/* A NEW LORD. Not a headline — succession happens somewhere in the world every few years and
 * a toast for each one would bury the wars — but it belongs in the log, with both names. */
/* A TOWN CHANGES HANDS. This is a headline: a border moving is the biggest thing that
 * happens in a war, and until now nothing in the game could do it — a war could only kill
 * people, so the political map never moved except by rebellion. */
/* TWO CROWNS COME TO TERMS. A headline, like a war: an alliance changes who a third kingdom
 * dares to attack, and it is the only good news the diplomacy model can produce. */
void mb_chron_ally(int a, int b)
{
    Village *c = &mb_v[mb_k[a].capital];
    push(EV_ALLY, a, b, 0, mb_k[a].name, c->x, c->y, mb_k[b].name);
}
void mb_chron_taken(int v, int by)
{
    push(EV_TAKEN, v, by, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y, mb_k[by].name);
}
void mb_chron_lord(int v)
{
    push(EV_LORD, v, 0, 0, mb_v[v].lord_name, mb_v[v].x, mb_v[v].y, mb_v[v].name);
}
void mb_chron_war(int a, int b)
{
    Village *c = &mb_v[mb_k[a].capital];
    push(EV_WAR, a, b, 0, mb_k[a].name, c->x, c->y, mb_k[b].name);
}
void mb_chron_peace(int a, int b)
{
    Village *c = &mb_v[mb_k[a].capital];
    push(EV_PEACE, a, b, 0, mb_k[a].name, c->x, c->y, mb_k[b].name);
}
void mb_chron_rebel(int v, int from, int to)
{
    push(EV_REBEL, from, to, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y, mb_k[to].name);
}
/* --- WHAT GETS INTO THE RING -------------------------------------------------
 *
 * Restricting births to the civ species and keeping them out of the toast was not enough, and
 * filtering them out of the LOG VIEW was not either: measured at year 250, ninety-three of the
 * ninety-six entries were births and deaths, so the three surviving lines of actual news were
 * all from the same year, and every war, founding, rebellion and strike of two and a half
 * centuries had already been pushed off the end of the ring.
 *
 * A world of two hundred people turns over constantly. An ordinary birth and an ordinary death
 * are the WEATHER of the simulation, not its history, and both are already legible where they
 * belong — in the world, and on a soul card. So they never enter the ring at all, and a death
 * only does when it is a story: killed in battle, taken by plague, eaten, or lost to a
 * disaster. The ring then holds what a chronicle should hold.
 */
void mb_chron_birth(int child, int parent)
{
    (void)child; (void)parent;
}
void mb_chron_death(int u, int cause)
{
    if (mb_u[u].sp >= SP_CIV_N) return;
    if (cause != CAUSE_SLAIN && cause != CAUSE_PLAGUE
        && cause != CAUSE_DISASTER && cause != CAUSE_EATEN) return;
    push(EV_DEATH, u, 0, cause, mb_name_person((uint32_t)u * 7919u + mb_w.seed),
         mb_u[u].x >> 4, mb_u[u].y >> 4, 0);
}
void mb_chron_legend(int u, int why)
{
    push(EV_LEGEND, u, 0, why, mb_name_person((uint32_t)u * 7919u + mb_w.seed),
         mb_u[u].x >> 4, mb_u[u].y >> 4, 0);
}
void mb_chron_disaster(const char *what, int x, int y)
{
    push(EV_DISASTER, 0, 0, 0, 0, x, y, intern(what));
}
void mb_chron_age(const char *name)
{
    /* AN AGE HAS NO PLACE. This passed the middle of the map, which on most worlds is open
     * ocean — so every age change flew the camera out to sea and left you looking at water with
     * a caption about the Age of Iron. A headline that is about the whole world does not have
     * somewhere to take you; only a thing that happened to a person or a town does. Zero is the
     * ring's "no location", which mb_chron_where() already reports as unreachable. */
    push(EV_AGE, 0, 0, 0, 0, 0, 0, intern(name));
}

/* --- queries ------------------------------------------------------------ */

/* A grudge is just a count of the wars and rebellions between two kingdoms that
 * the ring still remembers. Old wounds fade with the ring, which is exactly the
 * behaviour a diplomacy model wants and costs nothing to maintain. */
int mb_chron_grudge(int a, int b)
{
    int g = 0;
    for (int i = 0; i < s_n; i++) {
        const Event *e = &s_ev[i];
        if (e->type == EV_WAR && ((e->a == a && e->b == b) || (e->a == b && e->b == a))) g++;
        if (e->type == EV_REBEL && ((e->a == a && e->b == b) || (e->a == b && e->b == a))) g += 2;
    }
    return g;
}

int mb_chron_count(void) { return s_n; }

/* IS THIS WORTH A LINE IN THE LOG? A world of two hundred people turns over constantly, so
 * births and deaths outnumber everything else by an order of magnitude and a 96-entry ring
 * fills with them inside a year — the log read as a parish register and the founding of a
 * kingdom, a war, a nuke and a plague were all pushed off the bottom of it.
 *
 * They stay in the RING, because the grudge model counts back through it and a death is still
 * a fact about the world. They are just not NEWS: the log skips an ordinary one and keeps the
 * deaths that are a story — killed in battle, taken by plague, lost to a disaster — along with
 * everything that was never routine in the first place. */
int mb_chron_notable(int back)
{
    if (back < 0 || back >= s_n) return 0;
    const Event *e = &s_ev[(s_head - 1 - back + NEV * 2) % NEV];
    if (e->type == EV_BIRTH) return 0;
    if (e->type == EV_DEATH)
        return e->mag == CAUSE_SLAIN || e->mag == CAUSE_PLAGUE
            || e->mag == CAUSE_DISASTER || e->mag == CAUSE_EATEN;
    return 1;
}

/* WHERE an entry happened, so the log can be navigated instead of merely read. The ring has
 * always stored coordinates — the auto-follow camera uses them — but nothing exposed them, so
 * the chronicle was a wall of text about places you could not get to. */
int mb_chron_where(int back, int *x, int *y)
{
    if (back < 0 || back >= s_n) return 0;
    int idx = (s_head - 1 - back + NEV * 2) % NEV;
    if (!s_ev[idx].x && !s_ev[idx].y) return 0;
    *x = s_ev[idx].x; *y = s_ev[idx].y;
    return 1;
}

/* `back` 0 is the newest. Renders on demand — the ring holds no text. */
void mb_chron_line(char *out, int n, int back, int *year)
{
    if (back < 0 || back >= s_n) { if (n) out[0] = 0; if (year) *year = 0; return; }
    int idx = (s_head - 1 - back + NEV * 2) % NEV;
    render(out, n, &s_ev[idx]);
    if (year) *year = s_ev[idx].year;
}

const char *mb_chron_toast(void) { return (s_toast_t > 0.0f && s_toast[0]) ? s_toast : 0; }

/* THE CAPTION FOR WHERE THE CAMERA WENT. The toast lasts 3.4 s and the auto-pan then glides
 * for another second or two, so a hands-off player watched the world arrive somewhere with
 * the explanation already gone — "I am jumped around to things that are usually not obvious".
 * This is the same line with no timer on it, and the HUD keeps it up for as long as nobody
 * is touching anything: if the world is driving, the world says where it took you. */
const char *mb_chron_headline(void) { return s_toast[0] ? s_toast : 0; }

/* THE ONE BEFORE IT. The HUD's top row was spending most of its width on "AGE OF IRON", which
 * changes about once a century and is already on the world screen — while the log, the thing
 * that actually tells you what is happening, had one line. Two lines of history is worth far
 * more than a caption you have read a hundred times. Returns the newest NOTABLE entry at or
 * after `back`, so the caller can walk the ring without knowing what is in it. */
const char *mb_chron_recent(int back)
{
    static char line[30];
    int seen = 0;
    for (int i = 0; i < s_n; i++) {
        if (!mb_chron_notable(i)) continue;
        if (seen++ != back) continue;
        int year = 0;
        mb_chron_line(line, sizeof line, i, &year);
        return line;
    }
    return 0;
}

int mb_chron_focus(int *x, int *y)
{
    if (!s_have_focus) return 0;
    *x = s_focus_x; *y = s_focus_y;
    s_have_focus = 0;              /* consumed: the camera follows once per event */
    return 1;
}

void mb_chron_step(float dt)
{
    if (s_toast_t > 0.0f) s_toast_t -= dt;
}
