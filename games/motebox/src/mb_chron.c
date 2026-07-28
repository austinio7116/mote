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

enum { NK_PLACE = 0, NK_KINGDOM, NK_PERSON };

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

enum { EV_FOUND = 0, EV_FALL, EV_BUILD, EV_WAR, EV_PEACE, EV_REBEL, EV_BIRTH,
       EV_DEATH, EV_LEGEND, EV_DISASTER, EV_AGE, EV_N };

typedef struct {
    uint8_t  type, a, b, mag;      /* a/b: village, kingdom or unit, per type */
    uint16_t year;
    uint16_t name;                 /* the id the line needs to render */
    uint8_t  x, y;                 /* where, for the camera to follow */
    uint16_t extra;
} Event;

#define NEV 96
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
           type == EV_LEGEND || type == EV_DISASTER || type == EV_AGE;
}

void mb_chron_init(void)
{
    memset(s_ev, 0, sizeof s_ev);
    s_n = s_head = 0;
    s_toast[0] = 0; s_toast_t = 0.0f; s_have_focus = 0;
}

static void render(char *out, int n, const Event *e);

static Event *push(int type, int a, int b, int mag, uint16_t name, int x, int y)
{
    Event *e = &s_ev[s_head];
    s_head = (s_head + 1) % NEV;
    if (s_n < NEV) s_n++;
    e->type = (uint8_t)type; e->a = (uint8_t)a; e->b = (uint8_t)b;
    e->mag = (uint8_t)mag; e->name = name;
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
        s_focus_x = (uint8_t)x; s_focus_y = (uint8_t)y; s_have_focus = 1;
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
        snprintf(out, (size_t)n, "%s is lost", n1);
        break;
    case EV_BUILD:
        mb_name_str(n1, sizeof n1, NK_PLACE, e->name);
        snprintf(out, (size_t)n, "%s: a %s", n1, mb_chron_word(e->extra));
        break;
    case EV_WAR:
        mb_name_str(n1, sizeof n1, NK_KINGDOM, e->name);
        mb_name_str(n2, sizeof n2, NK_KINGDOM, e->extra);
        snprintf(out, (size_t)n, "%s wars on %s", n1, n2);
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
    push(EV_FOUND, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y);
    mb_snd(SND_FOUND);
}
void mb_chron_fall(int v)
{
    push(EV_FALL, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y);
}
void mb_chron_build(int v, const char *what)
{
    Event *e = push(EV_BUILD, v, 0, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y);
    e->extra = intern(what);
    mb_snd(SND_BUILD);
}
void mb_chron_war(int a, int b)
{
    Village *c = &mb_v[mb_k[a].capital];
    Event *e = push(EV_WAR, a, b, 0, mb_k[a].name, c->x, c->y);
    e->extra = mb_k[b].name;
}
void mb_chron_peace(int a, int b)
{
    Village *c = &mb_v[mb_k[a].capital];
    Event *e = push(EV_PEACE, a, b, 0, mb_k[a].name, c->x, c->y);
    e->extra = mb_k[b].name;
}
void mb_chron_rebel(int v, int from, int to)
{
    Event *e = push(EV_REBEL, from, to, 0, mb_v[v].name, mb_v[v].x, mb_v[v].y);
    e->extra = mb_k[to].name;
}
void mb_chron_birth(int child, int parent)
{
    (void)parent;
    /* Births are the most common event by far and would flush every headline out
     * of a 96-entry ring within a year. They are recorded only for the civ
     * species, and never toast. */
    if (mb_u[child].sp >= SP_CIV_N) return;
    push(EV_BIRTH, child, 0, 0, mb_name_person((uint32_t)child * 7919u + mb_w.seed),
         mb_u[child].x >> 4, mb_u[child].y >> 4);
}
void mb_chron_death(int u, int cause)
{
    if (mb_u[u].sp >= SP_CIV_N) return;
    push(EV_DEATH, u, 0, cause, mb_name_person((uint32_t)u * 7919u + mb_w.seed),
         mb_u[u].x >> 4, mb_u[u].y >> 4);
}
void mb_chron_legend(int u, int why)
{
    push(EV_LEGEND, u, 0, why, mb_name_person((uint32_t)u * 7919u + mb_w.seed),
         mb_u[u].x >> 4, mb_u[u].y >> 4);
}
void mb_chron_disaster(const char *what, int x, int y)
{
    Event *e = push(EV_DISASTER, 0, 0, 0, 0, x, y);
    e->extra = intern(what);
}
void mb_chron_age(const char *name)
{
    Event *e = push(EV_AGE, 0, 0, 0, 0, MW / 2, MH / 2);
    e->extra = intern(name);
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

/* `back` 0 is the newest. Renders on demand — the ring holds no text. */
void mb_chron_line(char *out, int n, int back, int *year)
{
    if (back < 0 || back >= s_n) { if (n) out[0] = 0; if (year) *year = 0; return; }
    int idx = (s_head - 1 - back + NEV * 2) % NEV;
    render(out, n, &s_ev[idx]);
    if (year) *year = s_ev[idx].year;
}

const char *mb_chron_toast(void) { return (s_toast_t > 0.0f && s_toast[0]) ? s_toast : 0; }

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
