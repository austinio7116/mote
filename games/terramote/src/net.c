/* TerraMote — collaborative co-op over the 2P link (USB / LAN / Internet lobby).
 *
 * The HOST invites a friend into their SAVED world. Roles are explicit (picked
 * on the title screen), confirmed in the hello — never derived from
 * link_is_host(), which is meaningless over a bridge.
 *
 * Authority split:
 *   HOST  owns the world sim: enemies, drops, liquids, growth, chests, clock.
 *         Every world_set_fg/set_wall/set_liq on the host is captured and
 *         streamed to the guest as raw deltas — mining, building, tree falls,
 *         grass growth and lava->obsidian all replicate through ONE choke.
 *   GUEST owns its own player (movement, damage taken: victim-authoritative)
 *         and sends semantic ops up (mine/place/wall/door, enemy damage, chest
 *         edits). It applies its own ops locally as prediction (drops
 *         suppressed) and the host's raw echo confirms.
 *
 * World handover: the host RLE-packs the two tile planes band by band (the
 * exact save-file codec) plus the chest table, and streams them in 'm' chunks
 * with an FNV checksum — cross-arch float drift makes seed-regeneration
 * unusable (see GTA), and the whole point is the host's EDITED world anyway.
 */
#include "terra.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define NET_MAGIC  0xA5
#define NET_PROTO  1

#define BAND_ROWS  31
#define BANDS      8
#define BAND_BYTES (BAND_ROWS * WCOLS)
#define SECTIONS   17                 /* 8 bands x 2 planes + chest table */

enum { NS_OFF = 0, NS_WAIT_LINK, NS_HELLO, NS_SYNC, NS_PLAY, NS_FAILED };

static uint8_t s_ns;
static uint8_t s_host;                /* our role (menu choice) */
static uint8_t s_used_lobby;          /* teardown path differs for link_start */
static float   s_hello_t, s_wait_t;
static uint8_t s_got_hello;
static const char *s_phase = "";

uint8_t g_net_nodrops;

/* ---- peer view ---------------------------------------------------------- */
static struct {
    uint8_t present;
    float  x, y, tx, ty;              /* smoothed + latest target */
    int8_t facing;
    uint8_t clip, item, grap;
    float  aim, use_t, prev_use;
    float  gx, gy;
    int16_t hp;
    uint8_t app[8];                   /* hair_style,hair_col,skin,shirt,pants,armor[3] */
} s_peer;

/* ---- outbound ring ------------------------------------------------------ */
static uint8_t  s_txr[2048];          /* power of two */
static uint16_t s_txh, s_txt;
static int tx_space(void) { return (int)sizeof s_txr - 1 - ((uint16_t)(s_txh - s_txt) & (sizeof s_txr - 1)); }
static int tx_send(const uint8_t *m, int n) {
    if (tx_space() < n) return 0;
    for (int i = 0; i < n; i++) { s_txr[s_txh & (sizeof s_txr - 1)] = m[i]; s_txh++; }
    return 1;
}
static void tx_pump(void) {
    while (s_txt != s_txh) {
        int t = s_txt & (sizeof s_txr - 1);
        int run = (int)sizeof s_txr - t;
        int have = (uint16_t)(s_txh - s_txt);
        if (run > have) run = have;
        /* KEEP CALLS SMALL. Over a lobby link the OS netshim is all-or-nothing
         * per link_send: a message is accepted only if its WHOLE length fits
         * the 512B carry, else 0. Offering the ring's full run (up to 2KB)
         * returns 0 forever once the run outgrows the carry — the world
         * transfer wedges on its first chunks. 128B always fits eventually. */
        if (run > 128) run = 128;
        int w = mote->link_send(s_txr + t, run);
        if (w <= 0) return;
        s_txt = (uint16_t)(s_txt + w);
    }
}

/* little-endian field helpers */
static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static int      get16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ---- raw world-delta queue (host -> guest, lossless) -------------------- */
#define FGQ 256                       /* power of two */
static struct { uint8_t plane, v; int16_t c, r; } s_fgq[FGQ];
static uint16_t s_fgh, s_fgt;
static void fgq_push(uint8_t plane, int c, int r, uint8_t v) {
    /* coalesce: a later write to the same cell replaces the queued one */
    for (uint16_t i = s_fgt; i != s_fgh; i++) {
        int k = i & (FGQ - 1);
        if (s_fgq[k].plane == plane && s_fgq[k].c == c && s_fgq[k].r == r) { s_fgq[k].v = v; return; }
    }
    if ((uint16_t)(s_fgh - s_fgt) >= FGQ) return;   /* overflow: sim burst beyond any real case */
    int k = s_fgh & (FGQ - 1);
    s_fgq[k].plane = plane; s_fgq[k].v = v;
    s_fgq[k].c = (int16_t)c; s_fgq[k].r = (int16_t)r;
    s_fgh++;
}

/* ---- liquid dirty set (host -> guest, latest-value, self-healing) ------- */
#define LIQN 96
static struct { int16_t c, r; } s_liq[LIQN];
static int s_nliq;
static void liq_push(int c, int r) {
    for (int i = 0; i < s_nliq; i++)
        if (s_liq[i].c == c && s_liq[i].r == r) return;
    if (s_nliq < LIQN) { s_liq[s_nliq].c = (int16_t)c; s_liq[s_nliq].r = (int16_t)r; s_nliq++; }
    /* overflow: dropped cells heal via the slow background scan */
}
static int s_scan_r;                  /* background liquid row-scan cursor */

/* ---- world transfer ------------------------------------------------------ */
static uint32_t s_wtotal, s_wdone;    /* payload bytes (both directions) */
static uint32_t s_wfnv;               /* host's plane checksum, from 'W' */
static uint16_t s_wseq;               /* 'm' sequence */
/* host tx cursor */
static int s_txsec;                   /* section being sent (SECTIONS = done) */
static int s_txoff, s_txlen;          /* progress inside packed section */
static uint8_t s_txhdr[4];
static uint8_t s_peer_acked;
/* guest rx parser */
static uint8_t s_rxhdr[4];
static int s_rxhdrn, s_rxneed, s_rxgot;
static uint8_t s_got_meta, s_sync_done;
/* meta from 'W' */
static uint32_t s_mseed;
static uint16_t s_mtime;
static uint8_t  s_mflags;
static int16_t  s_mspawn_c, s_mspawn_r;

static uint32_t plane_fnv(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < WCOLS * WROWS; i++) { h ^= g_fgm[i]; h *= 16777619u; }
    for (int i = 0; i < WCOLS * WROWS; i++) { h ^= g_bgm[i]; h *= 16777619u; }
    return h;
}

static int pack_section(int sec) {    /* -> RLE length in save_scratch() */
    uint8_t *scr = save_scratch();
    if (sec < 16) {
        int band = sec >> 1, plane = sec & 1;
        const uint8_t *src = (plane ? g_bgm : g_fgm) + band * BAND_BYTES;
        return save_rle_pack(src, BAND_BYTES, scr, BAND_BYTES * 2 + 16);
    }
    return save_rle_pack((const uint8_t *)g_chests, (int)sizeof(g_chests), scr, BAND_BYTES * 2 + 16);
}

static void world_tx_begin(void) {
    s_wtotal = 0;
    for (int s = 0; s < SECTIONS; s++) s_wtotal += 4u + (uint32_t)pack_section(s);
    s_wdone = 0; s_wseq = 0;
    s_txsec = -1; s_txoff = 0; s_txlen = 0;
    s_peer_acked = 0;
    uint8_t m[21];
    m[0] = NET_MAGIC; m[1] = 'W';
    put32(m + 2, g_seed);
    put32(m + 6, plane_fnv());
    put16(m + 10, (int)(g_time * 65535.0f));
    m[12] = g_boss_down ? 1 : 0;
    put16(m + 13, g_pl.spawn_c);
    put16(m + 15, g_pl.spawn_r);
    put32(m + 17, s_wtotal);
    tx_send(m, 21);
}

/* stream as much of the packed world as the ring will take this frame */
static void world_tx_pump(void) {
    uint8_t *scr = save_scratch();
    for (;;) {
        if (s_txsec >= SECTIONS) return;                     /* all queued */
        if (s_txsec < 0 || s_txoff >= s_txlen + 4) {         /* next section */
            s_txsec++;
            if (s_txsec >= SECTIONS) return;
            s_txlen = pack_section(s_txsec);
            s_txoff = 0;
            s_txhdr[0] = (uint8_t)(s_txsec < 16 ? (s_txsec & 1) : 2);   /* plane: 0 fg, 1 bg, 2 chests */
            s_txhdr[1] = (uint8_t)(s_txsec < 16 ? (s_txsec >> 1) : 0);
            put16(s_txhdr + 2, s_txlen);
        }
        uint8_t m[5 + 96];
        int n = 0;
        while (n < 96 && s_txoff < s_txlen + 4) {
            m[5 + n++] = s_txoff < 4 ? s_txhdr[s_txoff] : scr[s_txoff - 4];
            s_txoff++;
        }
        if (!n) return;
        m[0] = NET_MAGIC; m[1] = 'm';
        put16(m + 2, s_wseq);
        m[4] = (uint8_t)n;
        if (tx_space() < 5 + n) { s_txoff -= n; return; }    /* ring full: retry next frame */
        tx_send(m, 5 + n);
        s_wseq++;
        s_wdone += (uint32_t)n;
    }
}

/* guest: one completed section lands in the planes / chest table */
static void world_rx_section(void) {
    uint8_t *scr = save_scratch();
    int plane = s_rxhdr[0], band = s_rxhdr[1];
    if (plane == 2) {
        save_rle_unpack(scr, s_rxgot, (uint8_t *)g_chests, (int)sizeof(g_chests));
    } else if (plane <= 1 && band < BANDS) {
        save_rle_unpack(scr, s_rxgot, (plane ? g_bgm : g_fgm) + band * BAND_BYTES, BAND_BYTES);
    }
}

static void net_fail(const char *msg) {
    if (getenv("TERRA_DBG")) {
        char b[80];
        snprintf(b, sizeof b, "net FAIL: %s (link=%d health=%d ns=%d)",
                 msg, mote->link_status(), mote->net_health(), s_ns);
        mote->log(b);
    }
    ui_toast(msg);
    s_ns = NS_FAILED;
}

static void world_rx_bytes(const uint8_t *d, int n) {
    while (n > 0) {
        if (s_rxhdrn < 4) {                                  /* section header */
            s_rxhdr[s_rxhdrn++] = *d++; n--;
            if (s_rxhdrn == 4) {
                s_rxneed = (int)(uint16_t)(s_rxhdr[2] | (s_rxhdr[3] << 8));
                s_rxgot = 0;
                if (s_rxneed > BAND_BYTES * 2 + 16) { net_fail("TRANSFER ERROR"); return; }
            }
            s_wdone++;
            continue;
        }
        int take = s_rxneed - s_rxgot;
        if (take > n) take = n;
        memcpy(save_scratch() + s_rxgot, d, (size_t)take);
        s_rxgot += take; d += take; n -= take;
        s_wdone += (uint32_t)take;
        if (s_rxgot >= s_rxneed) {
            world_rx_section();
            s_rxhdrn = 0;
        }
    }
    if (s_got_meta && s_wdone >= s_wtotal && !s_sync_done) {
        world_rebuild_caches();
        if (plane_fnv() != s_wfnv) { net_fail("TRANSFER ERROR"); return; }
        uint8_t m[2] = { NET_MAGIC, 'y' };
        tx_send(m, 2);
        s_sync_done = 1;
        if (getenv("TERRA_DBG")) mote->log("net: world received, fnv ok");
    }
}

/* ---- tiny senders -------------------------------------------------------- */
static void send_hello(void) {
    uint8_t m[14];
    m[0] = NET_MAGIC; m[1] = 'H';
    m[2] = NET_PROTO; m[3] = s_host;
    m[4] = g_pl.hair_style; m[5] = g_pl.hair_col; m[6] = g_pl.skin_col;
    m[7] = g_pl.shirt_col;  m[8] = g_pl.pants_col;
    m[9] = g_pl.armor[0]; m[10] = g_pl.armor[1]; m[11] = g_pl.armor[2];
    put16(m + 12, g_pl.maxhp);
    tx_send(m, 14);
}

static void send_state(void) {
    uint8_t m[23];
    m[0] = NET_MAGIC; m[1] = 'S';
    put16(m + 2, (int)g_pl.x);
    put16(m + 4, (int)g_pl.y);
    m[6] = (uint8_t)((g_pl.facing < 0 ? 1 : 0) | (g_pl.on_ground ? 2 : 0) |
                     (g_state == GS_DEAD ? 4 : 0));
    m[7] = player_clip_id();
    m[8] = g_pl.inv[g_pl.hot].item;
    m[9] = (uint8_t)(int8_t)(player_aim() * 40.0f);
    m[10] = (uint8_t)mote_clampf(g_pl.use_t * 60.0f, 0, 255);
    put16(m + 11, g_pl.hp);
    m[13] = g_pl.grap;
    put16(m + 14, (int)g_pl.grap_x);
    put16(m + 16, (int)g_pl.grap_y);
    put16(m + 18, (int)(g_time * 65535.0f));
    m[20] = g_boss_down ? 1 : 0;
    m[21] = m[22] = 0;
    tx_send(m, 23);
}

static void send_op(uint8_t op, int c, int r, uint8_t arg) {
    uint8_t m[8];
    m[0] = NET_MAGIC; m[1] = 'e'; m[2] = op;
    put16(m + 3, c); put16(m + 5, r);
    m[7] = arg;
    tx_send(m, 8);
}

static void flush_fg_deltas(void) {
    while (s_fgt != s_fgh) {
        uint8_t m[3 + 24 * 6];
        int n = 0;
        uint16_t t = s_fgt;
        while (t != s_fgh && n < 24) {
            int k = t & (FGQ - 1);
            uint8_t *p = m + 3 + n * 6;
            p[0] = s_fgq[k].plane;
            put16(p + 1, s_fgq[k].c); put16(p + 3, s_fgq[k].r);
            p[5] = s_fgq[k].v;
            n++; t++;
        }
        m[0] = NET_MAGIC; m[1] = 'f'; m[2] = (uint8_t)n;
        if (!tx_send(m, 3 + n * 6)) return;                 /* ring full: keep queued */
        s_fgt = t;
    }
}

static void flush_liq_deltas(void) {
    if (!s_nliq) return;
    uint8_t m[3 + 24 * 5];
    int n = 0;
    for (int i = 0; i < s_nliq && n < 24; i++, n++) {
        uint8_t b = bg_at(s_liq[i].c, s_liq[i].r);
        uint8_t *p = m + 3 + n * 5;
        put16(p, s_liq[i].c); put16(p + 2, s_liq[i].r);
        p[4] = (uint8_t)(BG_LIQ(b) | (BG_IS_LAVA(b) ? 8 : 0));
    }
    m[0] = NET_MAGIC; m[1] = 'L'; m[2] = (uint8_t)n;
    if (!tx_send(m, 3 + n * 5)) return;
    if (n < s_nliq) memmove(s_liq, s_liq + n, (size_t)(s_nliq - n) * sizeof s_liq[0]);
    s_nliq -= n;
}

/* slow background row-scan: re-sends liquid truth around the players so any
 * delta the dirty set dropped (or a blip ate) heals within a few seconds */
static void liq_scan_step(void) {
    float px, py;
    int anchors = 1 + (net_peer_pos(&px, &py) ? 1 : 0);
    int which = (s_scan_r / 28) & 1;
    float ax = which && anchors == 2 ? px : g_pl.x;
    float ay = which && anchors == 2 ? py : g_pl.y;
    int r = (int)ay / TILE - 14 + (s_scan_r % 28);
    int c0 = mote_clampi((int)ax / TILE - 15, 0, WCOLS - 1);
    s_scan_r++;
    if ((unsigned)r >= WROWS) return;
    uint8_t m[3 + 24 * 5];
    int n = 0;
    for (int c = c0; c < c0 + 30 && c < WCOLS && n < 24; c++) {
        uint8_t b = bg_at(c, r);
        if (!BG_LIQ(b)) continue;
        uint8_t *p = m + 3 + n * 5;
        put16(p, c); put16(p + 2, r);
        p[4] = (uint8_t)(BG_LIQ(b) | (BG_IS_LAVA(b) ? 8 : 0));
        n++;
    }
    if (!n) return;
    m[0] = NET_MAGIC; m[1] = 'L'; m[2] = (uint8_t)n;
    tx_send(m, 3 + n * 5);
}

static void send_enemies(void) {
    uint8_t m[3 + 20 * 9];
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES && n < 20; i++) {
        Enemy *e = &g_en[i];
        if (!e->kind) continue;
        uint8_t *p = m + 3 + n * 9;
        p[0] = (uint8_t)i; p[1] = e->kind;
        put16(p + 2, (int)e->x); put16(p + 4, (int)e->y);
        put16(p + 6, e->hp);
        p[8] = (uint8_t)((e->facing < 0 ? 1 : 0) | (e->on_ground ? 2 : 0) |
                         (e->phase ? 4 : 0) | (e->hurt_t > 0 ? 8 : 0));
        n++;
    }
    m[0] = NET_MAGIC; m[1] = 'E'; m[2] = (uint8_t)n;
    tx_send(m, 3 + n * 9);
}

/* ---- inbound dispatch ---------------------------------------------------- */
static float dist_gain(float x, float y) {
    float dx = x - g_pl.x, dy = y - g_pl.y;
    float d = sqrtf(dx * dx + dy * dy);
    float g = 1.0f - d / 220.0f;
    return g < 0 ? 0 : g;
}

static void apply_fg_delta(int c, int r, uint8_t v) {
    uint8_t old = fg_at(c, r);
    world_set_fg(c, r, v);
    if (old == v) return;
    float x = c * TILE + 4, y = r * TILE + 4;
    float g = dist_gain(x, y);
    if (g <= 0.05f) return;
    if (v == T_AIR && g_tiles[old].solid) {                 /* someone dug here */
        audio_sfx(old == T_STONE || old >= T_COPPER ? SFX_DIG_STONE : SFX_DIG, 0.7f * g);
        part_burst(x, y, rgb(150, 110, 70), 3, 35);
    } else if (v != T_AIR && old == T_AIR) {
        audio_sfx(SFX_PLACE, 0.6f * g);
    }
}

static void handle_hello(const uint8_t *m) {
    if (m[2] != NET_PROTO) { net_fail("VERSION MISMATCH"); return; }
    if (m[3] == s_host) {
        net_fail(s_host ? "BOTH HOSTING - ONE MUST JOIN" : "NOBODY HOSTING - NO WORLD");
        return;
    }
    memcpy(s_peer.app, m + 4, 8);
    player_net_palette(s_peer.app);
    s_peer.hp = 100;
    s_got_hello = 1;
}

static void handle_state(const uint8_t *m) {
    float nx = (float)get16(m + 2), ny = (float)get16(m + 4);
    if (!s_peer.present || fabsf(nx - s_peer.x) > 48.0f || fabsf(ny - s_peer.y) > 48.0f) {
        s_peer.x = nx; s_peer.y = ny;
    }
    s_peer.present = 1;
    s_peer.tx = nx; s_peer.ty = ny;
    uint8_t fl = m[6];
    s_peer.facing = (fl & 1) ? -1 : 1;
    s_peer.clip = m[7];
    s_peer.item = m[8];
    s_peer.aim = (float)(int8_t)m[9] / 40.0f;
    s_peer.prev_use = s_peer.use_t;
    s_peer.use_t = (float)m[10] / 60.0f;
    if (s_peer.use_t > s_peer.prev_use + 0.08f)             /* fresh swing */
        audio_sfx(SFX_SWING, 0.5f * dist_gain(s_peer.x, s_peer.y));
    s_peer.hp = (int16_t)get16(m + 11);
    if (fl & 4) s_peer.hp = 0;                              /* dead: hide */
    s_peer.grap = m[13];
    s_peer.gx = (float)get16(m + 14);
    s_peer.gy = (float)get16(m + 16);
    if (!s_host) {                                          /* host owns the clock */
        g_time = (float)(uint16_t)(m[18] | (m[19] << 8)) / 65535.0f;
        g_boss_down = m[20] & 1;
    }
}

static void handle_op(const uint8_t *m) {                   /* guest ops, host applies */
    if (!s_host) return;
    int c = get16(m + 3), r = get16(m + 5);
    switch (m[2]) {
    case NOP_MINE:  world_mine_tile(c, r); break;
    case NOP_PLACE: world_place_tile(c, r, m[7]); break;
    case NOP_WALL:  world_wall_op(c, r, m[7]); break;
    case NOP_DOOR:  toggle_door(c, r); break;
    }
}

static void handle_msg(const uint8_t *m, int len) {
    (void)len;
    switch (m[1]) {
    case 'H': handle_hello(m); break;
    case 'W':                                               /* guest: transfer meta */
        if (s_host) break;
        s_mseed = get32(m + 2);
        s_wfnv = get32(m + 6);
        s_mtime = (uint16_t)(m[10] | (m[11] << 8));
        s_mflags = m[12];
        s_mspawn_c = (int16_t)get16(m + 13);
        s_mspawn_r = (int16_t)get16(m + 15);
        s_wtotal = get32(m + 17);
        s_wdone = 0; s_rxhdrn = 0; s_wseq = 0;
        s_got_meta = 1;
        break;
    case 'm': {
        if (s_host) break;
        uint16_t seq = (uint16_t)(m[2] | (m[3] << 8));
        if (seq != s_wseq) { net_fail("TRANSFER ERROR"); break; }
        s_wseq++;
        world_rx_bytes(m + 5, m[4]);
        break;
    }
    case 'y': s_peer_acked = 1; break;
    case 'S': handle_state(m); break;
    case 'e': handle_op(m); break;
    case 'd':                                               /* guest dealt damage */
        if (s_host)
            npc_damage_at((float)get16(m + 2), (float)get16(m + 4),
                          (float)m[6], (float)m[7],
                          get16(m + 8), (float)get16(m + 10), m[12]);
        break;
    case 'r':                                               /* guest wants a shared drop */
        if (s_host) drops_add(m[2], m[3], (float)get16(m + 4), (float)get16(m + 6));
        break;
    case 'b':                                               /* guest used the eye */
        if (s_host && IS_NIGHT()) npc_spawn_boss();
        break;
    case 'f':                                               /* raw tile/wall deltas */
        if (s_host) break;
        for (int i = 0; i < m[2]; i++) {
            const uint8_t *p = m + 3 + i * 6;
            int c = get16(p + 1), r = get16(p + 3);
            if (p[0] == 0) apply_fg_delta(c, r, p[5]);
            else world_set_wall(c, r, p[5]);
        }
        break;
    case 'L':                                               /* liquid nibbles */
        if (s_host) break;
        for (int i = 0; i < m[2]; i++) {
            const uint8_t *p = m + 3 + i * 5;
            world_set_liq_raw(get16(p), get16(p + 2), p[4]);
        }
        break;
    case 'D':                                               /* drop spawned on host */
        if (s_host) break;
        if (m[2] < MAX_DROPS) {
            g_drops[m[2]] = (Drop){ m[3], m[4], (float)get16(m + 5), (float)get16(m + 7),
                                    (float)get16(m + 9), (float)get16(m + 11), 0 };
            if (getenv("TERRA_DBG")) {
                char b[48];
                snprintf(b, sizeof b, "net drop %d item=%d at %d,%d", m[2], m[3], get16(m + 5), get16(m + 7));
                mote->log(b);
            }
        }
        break;
    case 'c':                                               /* drop resolved on host */
        if (s_host) break;
        if (m[2] < MAX_DROPS) {
            Drop *d = &g_drops[m[2]];
            if (m[3] == 1 && d->item) {                     /* we collected it */
                inv_add(d->item, d->count);
                audio_sfx(d->item == I_COIN ? SFX_COIN : SFX_TICK, d->item == I_COIN ? 0.9f : 0.45f);
            }
            d->item = I_NONE;
        }
        break;
    case 'E': {
        if (s_host) break;
        npc_net_snapshot_begin();
        for (int i = 0; i < m[2]; i++) {
            const uint8_t *p = m + 3 + i * 9;
            npc_net_apply(p[0], p[1], (float)get16(p + 2), (float)get16(p + 4),
                          get16(p + 6), p[8]);
        }
        npc_net_snapshot_end();
        break;
    }
    case 'C': {                                             /* chest slot changed */
        Chest *ch = world_chest_at(get16(m + 2), get16(m + 4));
        if (ch && m[6] < CHEST_SLOTS) ch->s[m[6]] = (Slot){ m[7], m[8] };
        break;
    }
    case 'K': {                                             /* chest created/removed */
        if (s_host) break;
        int c = get16(m + 2), r = get16(m + 4);
        if (m[6]) { if (!world_chest_at(c, r)) world_chest_create(c, r); }
        else world_chest_remove(c, r);
        break;
    }
    case 'B':
        if (s_host) break;
        if (m[2] == 0) { ui_toast("THE EYE OF CTHULHU HAS AWOKEN!"); audio_sfx(SFX_ROAR, 1.0f); }
        else if (m[2] == 1) { ui_toast("THE EYE OF CTHULHU IS DEFEATED"); audio_sfx(SFX_ROAR, 1.0f); }
        else ui_toast("THE EYE FLEES...");
        break;
    case 'p':                                               /* peer's arrow, cosmetic */
        proj_add_net(m[2], (float)get16(m + 3), (float)get16(m + 5),
                     (float)get16(m + 7), (float)get16(m + 9), m[11]);
        break;
    case 'Q':
        if (s_host) {
            ui_toast("YOUR FRIEND LEFT");
            net_stop(0);                                    /* world carries on solo */
        } else {
            net_fail("HOST LEFT THE GAME");
        }
        break;
    }
}

/* expected total frame length for a (partially received) message */
static int want_len(const uint8_t *m, int have) {
    switch (m[1]) {
    case 'H': return 14;
    case 'W': return 21;
    case 'm': return have >= 5 ? 5 + m[4] : 5;
    case 'y': case 'Q': return 2;
    case 'S': return 23;
    case 'e': return 8;
    case 'd': return 13;
    case 'r': return 8;
    case 'b': return 3;
    case 'f': return have >= 3 ? 3 + m[2] * 6 : 3;
    case 'L': return have >= 3 ? 3 + m[2] * 5 : 3;
    case 'D': return 13;
    case 'c': return 4;
    case 'E': return have >= 3 ? 3 + m[2] * 9 : 3;
    case 'C': return 9;
    case 'K': return 7;
    case 'B': return 3;
    case 'p': return 12;
    }
    return -1;
}

static uint8_t s_msg[224];
static int s_msg_len;

static void rx_pump(void) {
    uint8_t chunk[192];
    int n;
    while ((n = mote->link_recv(chunk, (int)sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            uint8_t b = chunk[i];
            if (s_msg_len == 0) { if (b == NET_MAGIC) s_msg[s_msg_len++] = b; continue; }
            s_msg[s_msg_len++] = b;
            int want = want_len(s_msg, s_msg_len);
            if (want < 0 || want > (int)sizeof s_msg) { s_msg_len = 0; continue; }
            if (s_msg_len < want) continue;
            s_msg_len = 0;
            handle_msg(s_msg, want);
            if (s_ns == NS_OFF || s_ns == NS_FAILED) return;
        }
    }
}

/* ---- public: session lifecycle ------------------------------------------ */
int net_active(void)  { return s_ns == NS_PLAY; }
int net_is_host(void) { return s_ns == NS_PLAY && s_host; }
int net_guest(void)   { return s_ns == NS_PLAY && !s_host; }
int net_failed(void)  { return s_ns == NS_FAILED; }
int net_ready(void)   {
    return s_ns == NS_SYNC &&
           (s_host ? (s_txsec >= SECTIONS && s_peer_acked) : s_sync_done);
}

static void session_reset(int host) {
    memset(&s_peer, 0, sizeof s_peer);
    s_host = (uint8_t)host;
    s_got_hello = 0; s_hello_t = 0; s_wait_t = 0;
    s_msg_len = 0;
    s_txh = s_txt = 0;
    s_fgh = s_fgt = 0;
    s_nliq = 0; s_scan_r = 0;
    s_wtotal = s_wdone = 0; s_wseq = 0;
    s_got_meta = 0; s_sync_done = 0; s_peer_acked = 0;
    s_rxhdrn = 0;
    g_net_nodrops = 0;
    s_phase = "CONNECTING";
}

void net_begin(int host) {
    MoteNetCfg cfg = { "TerraMote", NET_PROTO, 0 };
    int lobby_host;
    session_reset(host);
    s_used_lobby = 1;
    if (mote->net_lobby(&cfg, &lobby_host) != MOTE_NET_CONNECTED) {
        s_ns = NS_OFF;
        g_state = GS_TITLE;
        return;
    }
    s_ns = NS_HELLO;
    g_state = GS_NET_SYNC;
}

void net_begin_direct(int host) {                           /* dev/test path */
    session_reset(host);
    s_used_lobby = 0;
    mote->link_start();
    s_ns = NS_WAIT_LINK;
    g_state = GS_NET_SYNC;
}

void net_stop(int notify) {
    if (s_ns == NS_OFF) return;
    if (notify) {
        uint8_t m[2] = { NET_MAGIC, 'Q' };
        tx_send(m, 2);
        tx_pump();
    }
    mote->link_stop();
    s_ns = NS_OFF;
}

const char *net_phase_text(void) { return s_phase; }
int net_progress(void) {
    if (!s_wtotal) return 0;
    uint32_t p = s_wdone * 100u / s_wtotal;
    return p > 100 ? 100 : (int)p;
}

int net_peer_pos(float *x, float *y) {
    if (s_ns != NS_PLAY || !s_peer.present || s_peer.hp <= 0) return 0;
    *x = s_peer.x; *y = s_peer.y;
    return 1;
}

/* meta for game.c when the guest enters play */
void net_guest_spawn(int *c, int *r);
void net_guest_spawn(int *c, int *r) { *c = s_mspawn_c; *r = s_mspawn_r; }
void net_apply_meta(void);
void net_apply_meta(void) {
    if (s_host) return;
    g_seed = s_mseed;
    g_time = (float)s_mtime / 65535.0f;
    g_boss_down = s_mflags & 1;
}
void net_enter_play(void);
void net_enter_play(void) { s_ns = NS_PLAY; s_phase = ""; }

/* ---- per-frame ------------------------------------------------------------ */
static float s_state_t, s_en_t, s_liq_t;

void net_tick(float dt) {
    if (s_ns == NS_OFF || s_ns == NS_FAILED) return;

    if (mote->link_status() != MOTE_LINK_CONNECTED && s_ns != NS_WAIT_LINK) {
        if (s_host && s_ns == NS_PLAY) { ui_toast("LINK LOST - PLAYING SOLO"); net_stop(0); }
        else net_fail("LINK LOST");
        return;
    }
    if (s_ns >= NS_HELLO && mote->net_health() == MOTE_NET_LOST) {
        if (s_host && s_ns == NS_PLAY) { ui_toast("LINK LOST - PLAYING SOLO"); net_stop(0); }
        else net_fail("LINK LOST");
        return;
    }

    tx_pump();
    rx_pump();
    if (s_ns == NS_OFF || s_ns == NS_FAILED) return;

    switch (s_ns) {
    case NS_WAIT_LINK:
        s_phase = "WAITING FOR LINK";
        s_wait_t += dt;
        if (mote->link_status() == MOTE_LINK_CONNECTED) { s_ns = NS_HELLO; s_wait_t = 0; }
        else if (s_wait_t > 60.0f) net_fail("NO LINK FOUND");
        break;
    case NS_HELLO:
        s_phase = "GREETING";
        s_hello_t -= dt;
        if (s_hello_t <= 0) { send_hello(); s_hello_t = 0.45f; }
        s_wait_t += dt;
        if (s_wait_t > 20.0f) { net_fail("NO ANSWER FROM PEER"); break; }
        if (s_got_hello) {
            s_ns = NS_SYNC;
            s_wait_t = 0;
            if (s_host) { world_tx_begin(); s_phase = "SENDING WORLD"; }
            else s_phase = "RECEIVING WORLD";
        }
        break;
    case NS_SYNC:
        if (s_host) {
            world_tx_pump();
            if (s_txsec >= SECTIONS) s_phase = "WAITING FOR FRIEND";
        }
        s_wait_t += dt;
        if (s_wait_t > 90.0f) net_fail("TRANSFER TIMED OUT");
        break;
    case NS_PLAY: {
        /* dev: periodic world checksum so two instances can be diffed live */
        static float s_fnv_t; static int s_dbg_on = -1;
        if (s_dbg_on < 0) s_dbg_on = getenv("TERRA_DBG") != 0;
        if (s_dbg_on) {
            s_fnv_t += dt;
            if (s_fnv_t > 2.0f) {
                s_fnv_t = 0;
                char b[64];
                snprintf(b, sizeof b, "net fnv=%08x peer=%d,%d",
                         (unsigned)plane_fnv(), (int)s_peer.x, (int)s_peer.y);
                mote->log(b);
            }
        }
        /* smooth the peer toward its latest reported position */
        if (s_peer.present) {
            float k = mote_clampf(dt * 10.0f, 0, 1);
            s_peer.x += (s_peer.tx - s_peer.x) * k;
            s_peer.y += (s_peer.ty - s_peer.y) * k;
        }
        s_state_t += dt;
        if (s_state_t >= 1.0f / 15.0f) { s_state_t = 0; send_state(); }
        if (s_host) {
            flush_fg_deltas();
            s_en_t += dt;
            if (s_en_t >= 1.0f / 10.0f) { s_en_t = 0; send_enemies(); }
            s_liq_t += dt;
            if (s_liq_t >= 1.0f / 12.0f) { s_liq_t = 0; flush_liq_deltas(); liq_scan_step(); }
        }
        break;
    }
    default: break;
    }
}

/* ---- capture hooks (host) ------------------------------------------------ */
void net_raw_fg(int c, int r, uint8_t t) {
    if (s_ns != NS_PLAY || !s_host) return;
    fgq_push(0, c, r, t);
}
void net_raw_wall(int c, int r, uint8_t w) {
    if (s_ns != NS_PLAY || !s_host) return;
    fgq_push(1, c, r, w);
}
void net_raw_liq(int c, int r, uint8_t packed) {
    (void)packed;
    if (s_ns != NS_PLAY || !s_host) return;
    liq_push(c, r);
}
void net_chest_created(int c, int r) {
    if (s_ns != NS_PLAY || !s_host) return;
    uint8_t m[7];
    m[0] = NET_MAGIC; m[1] = 'K';
    put16(m + 2, c); put16(m + 4, r);
    m[6] = 1;
    tx_send(m, 7);
}
void net_chest_removed(int c, int r) {
    if (s_ns != NS_PLAY || !s_host) return;
    uint8_t m[7];
    m[0] = NET_MAGIC; m[1] = 'K';
    put16(m + 2, c); put16(m + 4, r);
    m[6] = 0;
    tx_send(m, 7);
}
void net_drop_spawned(int slot) {
    if (s_ns != NS_PLAY || !s_host) return;
    Drop *d = &g_drops[slot];
    uint8_t m[13];
    m[0] = NET_MAGIC; m[1] = 'D';
    m[2] = (uint8_t)slot; m[3] = d->item; m[4] = d->count;
    put16(m + 5, (int)d->x); put16(m + 7, (int)d->y);
    put16(m + 9, (int)d->vx); put16(m + 11, (int)d->vy);
    tx_send(m, 13);
}
void net_drop_taken(int slot, int by_peer) {
    if (s_ns != NS_PLAY || !s_host) return;
    uint8_t m[4] = { NET_MAGIC, 'c', (uint8_t)slot, (uint8_t)(by_peer ? 1 : 0) };
    tx_send(m, 4);
}
void net_ev_boss_state(int what) {
    if (s_ns != NS_PLAY || !s_host) return;
    uint8_t m[3] = { NET_MAGIC, 'B', (uint8_t)what };
    tx_send(m, 3);
}

/* ---- player-action wrappers ---------------------------------------------- */
void net_mine_tile(int c, int r) {
    if (net_guest()) {
        send_op(NOP_MINE, c, r, 0);
        g_net_nodrops = 1;                                  /* prediction: host owns drops */
        world_mine_tile(c, r);
        g_net_nodrops = 0;
    } else {
        world_mine_tile(c, r);
    }
}
int net_place_tile(int c, int r, uint8_t tile) {
    if (net_guest()) {
        g_net_nodrops = 1;
        int ok = world_place_tile(c, r, tile);
        g_net_nodrops = 0;
        if (ok) send_op(NOP_PLACE, c, r, tile);
        return ok;
    }
    return world_place_tile(c, r, tile);
}
void net_wall_op(int c, int r, uint8_t w) {
    if (net_guest()) {
        send_op(NOP_WALL, c, r, w);
        g_net_nodrops = 1;
        world_wall_op(c, r, w);
        g_net_nodrops = 0;
    } else {
        world_wall_op(c, r, w);
    }
}
void net_toggle_door(int c, int r) {
    if (net_guest()) send_op(NOP_DOOR, c, r, 0);
    toggle_door(c, r);
}
void net_ev_dmg(float x, float y, float hw, float hh, int dmg, float kx, uint8_t el) {
    if (s_ns != NS_PLAY || s_host) return;
    uint8_t m[13];
    m[0] = NET_MAGIC; m[1] = 'd';
    put16(m + 2, (int)x); put16(m + 4, (int)y);
    m[6] = (uint8_t)mote_clampf(hw, 0, 255); m[7] = (uint8_t)mote_clampf(hh, 0, 255);
    put16(m + 8, dmg); put16(m + 10, (int)kx);
    m[12] = el;
    tx_send(m, 13);
}
void net_ev_proj(uint8_t kind, float x, float y, float vx, float vy, uint8_t el) {
    if (s_ns != NS_PLAY) return;
    uint8_t m[12];
    m[0] = NET_MAGIC; m[1] = 'p'; m[2] = kind;
    put16(m + 3, (int)x); put16(m + 5, (int)y);
    put16(m + 7, (int)vx); put16(m + 9, (int)vy);
    m[11] = el;
    tx_send(m, 12);
}
void net_ev_chest(int c, int r, int slot, uint8_t item, uint8_t count) {
    if (s_ns != NS_PLAY) return;
    uint8_t m[9];
    m[0] = NET_MAGIC; m[1] = 'C';
    put16(m + 2, c); put16(m + 4, r);
    m[6] = (uint8_t)slot; m[7] = item; m[8] = count;
    tx_send(m, 9);
}
void net_ev_boss_req(void) {
    if (s_ns != NS_PLAY || s_host) return;
    uint8_t m[3] = { NET_MAGIC, 'b', 0 };
    tx_send(m, 3);
}
void net_ev_drop_req(uint8_t item, int n, float x, float y) {
    if (s_ns != NS_PLAY || s_host) return;
    while (n > 0) {
        uint8_t m[8];
        int take = n > 99 ? 99 : n;
        m[0] = NET_MAGIC; m[1] = 'r'; m[2] = item; m[3] = (uint8_t)take;
        put16(m + 4, (int)x); put16(m + 6, (int)y);
        tx_send(m, 8);
        n -= take;
    }
}

/* ---- remote player rendering --------------------------------------------- */
void net_draw_remote(void) {
    if (s_ns != NS_PLAY || !s_peer.present || s_peer.hp <= 0) return;
    NetPeerDraw v = {
        .x = s_peer.x, .y = s_peer.y, .aim = s_peer.aim,
        .facing = s_peer.facing,
        .clip = s_peer.clip, .item = s_peer.item, .grap = s_peer.grap,
        .hidden = 0,
        .use_t = s_peer.use_t,
        .gx = s_peer.gx, .gy = s_peer.gy,
    };
    player_draw_net(&v);
}
void net_draw_remote_overlay(uint16_t *fb) {
    if (s_ns != NS_PLAY || !s_peer.present || s_peer.hp <= 0) return;
    NetPeerDraw v = {
        .x = s_peer.x, .y = s_peer.y, .aim = s_peer.aim,
        .facing = s_peer.facing,
        .clip = s_peer.clip, .item = s_peer.item, .grap = s_peer.grap,
        .hidden = 0,
        .use_t = s_peer.use_t,
        .gx = s_peer.gx, .gy = s_peer.gy,
    };
    player_draw_net_overlay(fb, &v);
}
