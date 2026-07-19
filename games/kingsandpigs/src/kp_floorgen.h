/* kp_floorgen.h — piece-graph floor generator (ported from assets/proto_gen.py).
 * Places carve-authored room pieces (kp_pieces_data.h) so they abut through
 * matching ports into an organic castle 4-5 rooms tall. Fills a char canvas;
 * game.c converts it to map bits + entities. Uses mote_rand() (seed first). */
#ifndef KP_FLOORGEN_H
#define KP_FLOORGEN_H
#include "kp_pieces_data.h"

#define KP_CW 60
#define KP_CH 44
#define KP_VEXTENT 30          /* hard cap on top-to-bottom extent (~4-5 rooms) */
#define KP_MAXPLACED 48
#define KP_MAXPORTS 8

typedef struct { char k; unsigned char a, b; } KpPort;    /* k: T B L R */
typedef struct { unsigned char pi, x, y; unsigned char used; } KpPlaced;

static int kg_rndi(int n) { return n > 0 ? (int)(mote_rand() % (unsigned)n) : 0; }

static int kp_ports(int pi, KpPort *out) {
    const KpPiece *p = &KP_PIECES[pi];
    int w = p->w, h = p->h, n = 0, i;
    /* top '^' / bottom 'v' runs (2-wide) */
    for (i = 0; i < w; i++) if (p->r[0][i] == '^' && (i == 0 || p->r[0][i-1] != '^'))
        { out[n].k='T'; out[n].a=i; out[n].b=i+1; n++; }
    for (i = 0; i < w; i++) if (p->r[h-1][i] == 'v' && (i == 0 || p->r[h-1][i-1] != 'v'))
        { out[n].k='B'; out[n].a=i; out[n].b=i+1; n++; }
    for (i = 0; i < h; i++) if (p->r[i][0] == '<' && (i == 0 || p->r[i-1][0] != '<'))
        { out[n].k='L'; out[n].a=i; out[n].b=i+1; n++; }
    for (i = 0; i < h; i++) if (p->r[i][w-1] == '>' && (i == 0 || p->r[i-1][w-1] != '>'))
        { out[n].k='R'; out[n].a=i; out[n].b=i+1; n++; }
    return n;
}
static char kp_opp(char k) { return k=='T'?'B':k=='B'?'T':k=='L'?'R':'L'; }

/* generator state */
static char kg_cv[KP_CH][KP_CW];
static KpPlaced kg_placed[KP_MAXPLACED];
static int kg_nplaced;

static int kg_fits(int x, int y, int w, int h) {
    if (x < 1 || y < 1 || x + w > KP_CW - 1 || y + h > KP_CH - 1) return 0;
    for (int i = 0; i < kg_nplaced; i++) {
        const KpPiece *op = &KP_PIECES[kg_placed[i].pi];
        int ox = kg_placed[i].x, oy = kg_placed[i].y, ow = op->w, oh = op->h;
        int ix = (x > ox ? x : ox), iy = (y > oy ? y : oy);
        int ix1 = (x+w < ox+ow ? x+w : ox+ow), iy1 = (y+h < oy+oh ? y+h : oy+oh);
        if (ix1 - ix > 0 && iy1 - iy > 0) {
            if (ix1 - ix <= 1 || iy1 - iy <= 1) continue;   /* shared 1-tile edge ok */
            return 0;
        }
    }
    return 1;
}

static void kg_stamp(int pi, int x, int y) {
    const KpPiece *p = &KP_PIECES[pi];
    for (int r = 0; r < p->h; r++)
        for (int c = 0; c < p->w; c++) {
            char ch = p->r[r][c];
            kg_cv[y+r][x+c] = (ch=='^'||ch=='v'||ch=='<'||ch=='>') ? '#' : ch;
        }
}

/* carve/seal one port on a placed piece */
static void kg_open(int idx, KpPort port) {
    const KpPiece *p = &KP_PIECES[kg_placed[idx].pi];
    int x = kg_placed[idx].x, y = kg_placed[idx].y, h = p->h, w = p->w;
    /* vertical ports keep a THIN PLANK across the shared row; doors open fully */
    if (port.k == 'T') for (int c=port.a;c<=port.b;c++) kg_cv[y][x+c]='=';
    else if (port.k == 'B') for (int c=port.a;c<=port.b;c++) kg_cv[y+h-1][x+c]='=';
    else if (port.k == 'L') for (int r=port.a;r<=port.b;r++) kg_cv[y+r][x]='.';
    else for (int r=port.a;r<=port.b;r++) kg_cv[y+r][x+w-1]='.';
}
static void kg_seal(int idx, KpPort port) {
    const KpPiece *p = &KP_PIECES[kg_placed[idx].pi];
    int x = kg_placed[idx].x, y = kg_placed[idx].y, h = p->h, w = p->w;
    if (port.k == 'T') {
        for (int c=port.a;c<=port.b;c++) kg_cv[y][x+c]='#';
        for (int rr=y+1; rr<=y+2 && rr<KP_CH; rr++)          /* drop orphan foothold */
            for (int c=x+port.a-1; c<=x+port.b+1; c++)
                if (c>=0 && c<KP_CW && (kg_cv[rr][c]=='='||kg_cv[rr][c]=='-')) kg_cv[rr][c]='.';
    } else if (port.k == 'B') for (int c=port.a;c<=port.b;c++) kg_cv[y+h-1][x+c]='#';
    else if (port.k == 'L') for (int r=port.a;r<=port.b;r++) kg_cv[y+r][x]='#';
    else for (int r=port.a;r<=port.b;r++) kg_cv[y+r][x+w-1]='#';
}

/* try to attach a side piece to placed[idx] via `port`; returns piece idx +
 * placement + the new piece's mating port, or -1 */
static int kg_attach(int idx, KpPort port, int want_side,
                     int *obx, int *oby, KpPort *obp, char req) {
    const KpPiece *pc = &KP_PIECES[kg_placed[idx].pi];
    int px = kg_placed[idx].x, py = kg_placed[idx].y, pw = pc->w, ph = pc->h;
    char want = kp_opp(port.k);
    int lo = (want_side==1) ? 0 : (want_side==2) ? KP_NSTART : KP_NSTART+KP_NEXIT;
    int hi = (want_side==1) ? KP_NSTART : (want_side==2) ? KP_NSTART+KP_NEXIT : KP_NPIECES;
    int off = kg_rndi(hi - lo);
    for (int t = 0; t < hi - lo; t++) {
        int bn = lo + (off + t) % (hi - lo);
        KpPort bports[KP_MAXPORTS]; int nb = kp_ports(bn, bports);
        if (req) {                          /* must also have a `req`-kind port */
            int has = 0;
            for (int j = 0; j < nb; j++) if (bports[j].k == req) has = 1;
            if (!has) continue;
        }
        int boff = kg_rndi(nb < 1 ? 1 : nb);
        for (int j = 0; j < nb; j++) {
            KpPort bp = bports[(boff + j) % nb];
            if (bp.k != want || (port.b-port.a) != (bp.b-bp.a)) continue;
            const KpPiece *bpc = &KP_PIECES[bn]; int bw = bpc->w, bh = bpc->h, bx, by;
            if (port.k=='B') { bx = px + port.a - bp.a; by = py + ph - 1; }
            else if (port.k=='T') { bx = px + port.a - bp.a; by = py - bh + 1; }
            else if (port.k=='R') { bx = px + pw - 1; by = py + port.a - bp.a; }
            else { bx = px - bw + 1; by = py + port.a - bp.a; }
            if (kg_fits(bx, by, bw, bh)) { *obx=bx; *oby=by; *obp=bp; return bn; }
        }
    }
    return -1;
}

static int kg_vextent(int extra_top, int extra_bot) {
    int mn = extra_top, mx = extra_bot;
    for (int i = 0; i < kg_nplaced; i++) {
        int a = kg_placed[i].y, b = a + KP_PIECES[kg_placed[i].pi].h;
        if (i == 0 && extra_top == extra_bot) { mn = a; mx = b; }
        if (a < mn) mn = a; if (b > mx) mx = b;
    }
    return mx - mn;
}

/* frontier of open ports */
typedef struct { int idx; KpPort port; } KgFront;

/* fill kg_cv with a floor; returns number of pieces placed */
static int kp_gen(void) {
    for (int r = 0; r < KP_CH; r++) for (int c = 0; c < KP_CW; c++) kg_cv[r][c] = ' ';
    kg_nplaced = 0;
    KgFront front[256]; int nfront = 0;

    int spi = kg_rndi(KP_NSTART);                          /* a start piece */
    int sw = KP_PIECES[spi].w, sh = KP_PIECES[spi].h;
    int sx = KP_CW/2 - sw/2, sy = 2;
    kg_placed[kg_nplaced].pi = spi; kg_placed[kg_nplaced].x = sx;
    kg_placed[kg_nplaced].y = sy;  kg_placed[kg_nplaced].used = 0;
    kg_stamp(spi, sx, sy);
    kg_nplaced = 1;
    int exit_placed = 0;

    /* ---- vertical spine: chain rooms that keep a bottom port open, straight
     * down from the start, then cap it with the exit. This guarantees a
     * reachable exit at the bottom before the fill phase boxes things in. ---- */
    {
        int cur = 0;
        KpPort sports[KP_MAXPORTS]; int nsc = kp_ports(spi, sports);
        KpPort down = {0,0,0}; int have = 0;
        for (int i = 0; i < nsc; i++) if (sports[i].k == 'B') { down = sports[i]; have = 1; }
        while (have && kg_nplaced < KP_MAXPLACED) {
            int curbot = kg_placed[cur].y + KP_PIECES[kg_placed[cur].pi].h;
            int bx, by; KpPort bp;
            if (curbot - sy >= KP_VEXTENT - 9) {          /* deep enough — place exit */
                int en2 = kg_attach(cur, down, 2, &bx, &by, &bp, 0);
                if (en2 >= 0) {
                    int bi = kg_nplaced; kg_placed[bi].pi=en2; kg_placed[bi].x=bx; kg_placed[bi].y=by;
                    kg_placed[bi].used=0; kg_stamp(en2,bx,by); kg_open(cur,down); kg_open(bi,bp);
                    kg_nplaced++; exit_placed = 1;
                }
                break;
            }
            int bn = kg_attach(cur, down, 0, &bx, &by, &bp, 'B');   /* a room that continues down */
            if (bn < 0 || by < sy) {                        /* can't continue — exit here */
                int en2 = kg_attach(cur, down, 2, &bx, &by, &bp, 0);
                if (en2 >= 0) {
                    int bi = kg_nplaced; kg_placed[bi].pi=en2; kg_placed[bi].x=bx; kg_placed[bi].y=by;
                    kg_placed[bi].used=0; kg_stamp(en2,bx,by); kg_open(cur,down); kg_open(bi,bp);
                    kg_nplaced++; exit_placed = 1;
                }
                break;
            }
            int bi = kg_nplaced; kg_placed[bi].pi=bn; kg_placed[bi].x=bx; kg_placed[bi].y=by;
            kg_placed[bi].used=0; kg_stamp(bn,bx,by); kg_open(cur,down); kg_open(bi,bp);
            kg_nplaced++;
            /* add this spine room's OTHER ports to the frontier; find its next 'B' */
            KpPort q[KP_MAXPORTS]; int nq = kp_ports(bn, q); have = 0;
            for (int i = 0; i < nq; i++) {
                if (q[i].k==bp.k && q[i].a==bp.a) continue;
                if (q[i].k=='B' && !have) { down = q[i]; have = 1; }
                else if (nfront < 256) { front[nfront].idx=bi; front[nfront].port=q[i]; nfront++; }
            }
            cur = bi;
        }
    }

    /* the start's remaining (non-spine) ports join the fill frontier */
    KpPort sp[KP_MAXPORTS]; int nsp = kp_ports(spi, sp);
    for (int i = 0; i < nsp; i++) {
        int consumed = 0;
        /* the spine consumed one 'B'; skip a B port already opened */
        if (sp[i].k == 'B' && kg_cv[sy + KP_PIECES[spi].h - 1][sx + sp[i].a] != '#') consumed = 1;
        if (!consumed && nfront < 256) { front[nfront].idx = 0; front[nfront].port = sp[i]; nfront++; }
    }

    int target = 14, tries = 0;
    while (nfront > 0 && kg_nplaced < target && tries < 600) {
        tries++;
        int ext = kg_vextent(0, 0);
        int near_cap = ext >= KP_VEXTENT - 6;
        int want_down = (!near_cap) && (kg_rndi(100) < 40);
        /* pick a frontier port matching the desired direction */
        int pick = -1, scan = kg_rndi(nfront);
        for (int s = 0; s < nfront; s++) {
            int fi = (scan + s) % nfront;
            int isv = (front[fi].port.k=='T' || front[fi].port.k=='B');
            if (isv == want_down) { pick = fi; break; }
        }
        if (pick < 0) pick = kg_rndi(nfront);
        KgFront f = front[pick];
        front[pick] = front[--nfront];

        int bx, by; KpPort bp;
        int bn = kg_attach(f.idx, f.port, 0, &bx, &by, &bp, 0);
        if (bn < 0) continue;
        if (by < sy) continue;                 /* keep the start room at the top */
        if (kg_vextent(by, by + KP_PIECES[bn].h) > KP_VEXTENT) continue;
        if (kg_nplaced >= KP_MAXPLACED) break;
        int bi = kg_nplaced;
        kg_placed[bi].pi = bn; kg_placed[bi].x = bx; kg_placed[bi].y = by; kg_placed[bi].used = 0;
        kg_stamp(bn, bx, by);
        kg_open(f.idx, f.port);
        kg_open(bi, bp);
        kg_nplaced++;
        KpPort q[KP_MAXPORTS]; int nq = kp_ports(bn, q);
        for (int i = 0; i < nq; i++)
            if (!(q[i].k==bp.k && q[i].a==bp.a) && nfront < 256)
                { front[nfront].idx = bi; front[nfront].port = q[i]; nfront++; }
    }

    /* fallback exit (only if the spine didn't place one): try every open
     * non-top port deepest-first until one places (marking tried ports 'X') */
    for (int guard = 0; !exit_placed && guard < nfront && kg_nplaced < KP_MAXPLACED; guard++) {
        int best = -1, bestY = -2;
        for (int i = 0; i < nfront; i++) {
            if (front[i].port.k == 'T' || front[i].port.k == 'X') continue;
            int yy = kg_placed[front[i].idx].y;
            if (yy > bestY) { bestY = yy; best = i; }
        }
        if (best < 0) break;
        int bx, by; KpPort bp;
        int bn = kg_attach(front[best].idx, front[best].port, 2, &bx, &by, &bp, 0);
        if (bn >= 0 && by >= sy) {
            exit_placed = 1;
            int bi = kg_nplaced;
            kg_placed[bi].pi = bn; kg_placed[bi].x = bx; kg_placed[bi].y = by; kg_placed[bi].used=0;
            kg_stamp(bn, bx, by); kg_open(front[best].idx, front[best].port); kg_open(bi, bp);
            kg_nplaced++;
            front[best] = front[--nfront];
            break;
        }
        front[best].port.k = 'X';          /* tried; skip on the next pass */
    }
    /* seal every remaining open port */
    for (int i = 0; i < nfront; i++) kg_seal(front[i].idx, front[i].port);
    /* remove any plank tucked directly under solid brick: it's unstandable
     * (no headroom) and reads as a stray ledge stuck to the ceiling. A real
     * drop-plank always has open space above it, so this never hits those. */
    for (int r = 1; r < KP_CH; r++)
        for (int c = 0; c < KP_CW; c++)
            if ((kg_cv[r][c] == '=' || kg_cv[r][c] == '-') && kg_cv[r-1][c] == '#')
                kg_cv[r][c] = '.';
    return kg_nplaced;
}

#endif
