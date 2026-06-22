/*
 * chess — 3D chess on Mote. Board state + AI come from the vendored Chal engine
 * (chal.c). Pieces are procedural lathe/revolution meshes built at load and
 * stored in the load-time arena. You play White (near side); Black is the Chal
 * search. Moves animate (lift, glide, drop). Orbit the camera with LB/RB + the
 * shoulder modifiers.
 *
 * Controls: D-pad = move cursor · A = pick up / drop · B = cancel selection
 *           LB/RB = orbit camera · UP/DOWN while holding B = pitch/zoom · MENU = quit
 *
 * Style note — uses scene_camera(), so objects are added at WORLD coordinates
 * (no v3_sub(pos, cam) anywhere), and mote_draw_ex() builds the MoteObject.
 */
#include "mote_api.h"
#include "chal.h"
#include "mote_build.h"
#include <math.h>

MOTE_GAME_MODULE();
#ifdef MOTE_MODULE_BUILD
#include "mote_module.h"
MOTE_MODULE_HEADER();
#endif

#define TAU 6.2831853f

/* Chal time hook, routed through mote->micros (chal.c calls chess_time_ms). */
int64_t chess_time_ms(void) {
    return (int64_t)(mote->micros() / 1000ull);
}

/* ============================================================
 * Procedural Staunton piece meshes (revolution + box finials)
 * ============================================================ */

#define PIECE_SEGMENTS  10
#define PIECE_SCALE     1.6f      /* model half-extent: profiles may reach ~1.6 tall */
#define PIECE_MAX_VERTS 200
#define PIECE_MAX_FACES 360

/* A mesh under construction, owning its own vertex/face storage. */
typedef struct {
    MeshVert verts[PIECE_MAX_VERTS];
    MeshFace faces[PIECE_MAX_FACES];
    Mesh     mesh;
    int      vert_count;
    int      face_count;
} PieceMesh;

static PieceMesh piece_mesh[6][2];   /* [type - 1][color] */

static MeshVert board_verts[81];
static MeshFace board_faces[128];
static uint16_t board_fcol[128];     /* per-face colour (alternating squares) */
static Mesh     board_mesh;

/* Decode a quantised piece vertex back to world units. */
static Vec3 piece_vert_world(MeshVert v) {
    return v3((float)v.x * PIECE_SCALE / 127.0f,
              (float)v.y * PIECE_SCALE / 127.0f,
              (float)v.z * PIECE_SCALE / 127.0f);
}

/* Append a vertex (world units, quantised into int8). Returns its index. */
static int piece_add_vert(PieceMesh *m, float x, float y, float z) {
    if (m->vert_count >= PIECE_MAX_VERTS) return m->vert_count - 1;
    MeshVert *v = &m->verts[m->vert_count];
    v->x = (int8_t)(x / PIECE_SCALE * 127);
    v->y = (int8_t)(y / PIECE_SCALE * 127);
    v->z = (int8_t)(z / PIECE_SCALE * 127);
    return m->vert_count++;
}

/* Append a triangle, computing its outward normal from the quantised verts. */
static void piece_add_face(PieceMesh *m, int a, int b, int c, uint16_t col) {
    if (m->face_count >= PIECE_MAX_FACES) return;
    Vec3 pa = piece_vert_world(m->verts[a]);
    Vec3 pb = piece_vert_world(m->verts[b]);
    Vec3 pc = piece_vert_world(m->verts[c]);
    Vec3 n = v3_norm(v3_cross(v3_sub(pb, pa), v3_sub(pc, pa)));

    MeshFace *f = &m->faces[m->face_count++];
    f->a = (uint8_t)a;
    f->b = (uint8_t)b;
    f->c = (uint8_t)c;
    f->nx = (int8_t)(n.x * 127);
    f->ny = (int8_t)(n.y * 127);
    f->nz = (int8_t)(n.z * 127);
    m->mesh.color = col;   /* pieces are single-colour: colour lives on the Mesh now */
}

/* Axis-aligned box (12 faces) — finials (king cross, queen coronet spikes). */
static void piece_add_box(PieceMesh *m, float cx, float cy, float cz,
                          float hx, float hy, float hz, uint16_t col) {
    int base = m->vert_count;
    piece_add_vert(m, cx - hx, cy - hy, cz - hz);
    piece_add_vert(m, cx + hx, cy - hy, cz - hz);
    piece_add_vert(m, cx + hx, cy - hy, cz + hz);
    piece_add_vert(m, cx - hx, cy - hy, cz + hz);
    piece_add_vert(m, cx - hx, cy + hy, cz - hz);
    piece_add_vert(m, cx + hx, cy + hy, cz - hz);
    piece_add_vert(m, cx + hx, cy + hy, cz + hz);
    piece_add_vert(m, cx - hx, cy + hy, cz + hz);

    int faces[12][3] = {
        {0,5,4},{0,1,5},{1,6,5},{1,2,6},{2,7,6},{2,3,7},
        {3,4,7},{3,0,4},{4,5,6},{4,6,7},{0,7,3},{0,4,7}
    };
    for (int i = 0; i < 12; i++)
        piece_add_face(m, base + faces[i][0], base + faces[i][1], base + faces[i][2], col);
}

/* Lathe a profile of {radius, height} pairs around the Y axis. A final radius
 * < 0.03 marks the apex (point); otherwise the top is capped flat (rook). This
 * RESETS the mesh (vert/face counts to 0). */
static void piece_revolve(PieceMesh *m, const float *prof, int prof_count,
                          uint16_t col, int segments) {
    m->vert_count = 0;
    m->face_count = 0;

    int v0 = m->vert_count;
    int has_apex = prof[(prof_count - 1) * 2] < 0.03f;
    int rings = has_apex ? prof_count - 1 : prof_count;

    /* ring vertices */
    for (int i = 0; i < rings; i++) {
        float r = prof[i * 2];
        float y = prof[i * 2 + 1];
        for (int s = 0; s < segments; s++) {
            float a = s * TAU / segments;
            piece_add_vert(m, r * cosf(a), y, r * sinf(a));
        }
    }

    /* side quads between adjacent rings */
    for (int i = 0; i < rings - 1; i++) {
        for (int s = 0; s < segments; s++) {
            int s2 = (s + 1) % segments;
            int a = v0 + i * segments + s;
            int b = v0 + i * segments + s2;
            int c = v0 + (i + 1) * segments + s;
            int d = v0 + (i + 1) * segments + s2;
            piece_add_face(m, a, d, b, col);
            piece_add_face(m, a, c, d, col);
        }
    }

    /* top: a point (apex) or a flat cap (open-top rook) */
    if (has_apex) {
        int apex = piece_add_vert(m, 0, prof[(prof_count - 1) * 2 + 1], 0);
        int base = v0 + (rings - 1) * segments;
        for (int s = 0; s < segments; s++) {
            int s2 = (s + 1) % segments;
            piece_add_face(m, base + s2, base + s, apex, col);
        }
    } else {
        int top = piece_add_vert(m, 0, prof[(rings - 1) * 2 + 1], 0);
        int ring = v0 + (rings - 1) * segments;
        for (int s = 0; s < segments; s++) {
            int s2 = (s + 1) % segments;
            piece_add_face(m, top, ring + s2, ring + s, col);
        }
    }

    /* bottom cap */
    int center = piece_add_vert(m, 0, prof[1], 0);
    for (int s = 0; s < segments; s++) {
        int s2 = (s + 1) % segments;
        piece_add_face(m, center, v0 + s, v0 + s2, col);
    }

    m->mesh.verts = m->verts;
    m->mesh.faces = m->faces;
    m->mesh.nverts = m->vert_count;
    m->mesh.nfaces = m->face_count;
    m->mesh.scale = PIECE_SCALE;
    m->mesh.bound_r = 1.7f;
}

/* Knight: revolved base + an extruded horse-head silhouette (forward = +z). */
static const float KNIGHT_SILHOUETTE[] = {
    -0.16f,0.30f, -0.23f,0.50f, -0.16f,0.66f, -0.02f,0.76f,
     0.17f,0.71f,  0.30f,0.58f,  0.37f,0.46f,  0.26f,0.40f,
     0.10f,0.36f, -0.05f,0.32f
};

static void build_knight(PieceMesh *m, uint16_t col) {
    static const float base[] = { 0.27f,0, 0.35f,0.05f, 0.30f,0.12f, 0.22f,0.22f, 0.20f,0.32f };
    piece_revolve(m, base, 5, col, 8);

    const int silhouette_pts = 10;
    float half_width = 0.155f;

    int right = m->vert_count;
    for (int i = 0; i < silhouette_pts; i++)
        piece_add_vert(m, half_width, KNIGHT_SILHOUETTE[i * 2 + 1], KNIGHT_SILHOUETTE[i * 2]);
    int left = m->vert_count;
    for (int i = 0; i < silhouette_pts; i++)
        piece_add_vert(m, -half_width, KNIGHT_SILHOUETTE[i * 2 + 1], KNIGHT_SILHOUETTE[i * 2]);

    /* fan-fill each side face */
    for (int i = 1; i < silhouette_pts - 1; i++) {
        piece_add_face(m, right + 0, right + i, right + i + 1, col);
        piece_add_face(m, left + 0, left + i + 1, left + i, col);
    }
    /* rim joining the two silhouettes */
    for (int i = 0; i < silhouette_pts; i++) {
        int j = (i + 1) % silhouette_pts;
        piece_add_face(m, right + i, left + i, left + j, col);
        piece_add_face(m, right + i, left + j, right + j, col);
    }

    piece_add_box(m,  0.10f, 0.80f, -0.06f, 0.05f, 0.10f, 0.05f, col);   /* ears */
    piece_add_box(m, -0.10f, 0.80f, -0.06f, 0.05f, 0.10f, 0.05f, col);

    m->mesh.verts = m->verts;
    m->mesh.faces = m->faces;
    m->mesh.nverts = m->vert_count;
    m->mesh.nfaces = m->face_count;
    m->mesh.scale = PIECE_SCALE;
    m->mesh.bound_r = 1.7f;
}

/* Clamp 0..1 float RGB to an RGB565 colour. */
static uint16_t shade(float r, float g, float b) {
    int R = (int)(r * 255);
    int G = (int)(g * 255);
    int B = (int)(b * 255);
    if (R > 255) R = 255;
    if (G > 255) G = 255;
    if (B > 255) B = 255;
    return MOTE_RGB565(R, G, B);
}

static void build_pieces(void) {
    /* rounded narrow feet (widest a touch up from the base, not a flat wide disc) */
    static const float pawn[]   = {0.26f,0,0.34f,0.05f,0.30f,0.11f,0.17f,0.20f,0.15f,0.38f,0.22f,0.45f,0.15f,0.50f,0.205f,0.61f,0.16f,0.68f,0.0f,0.83f};
    static const float rook[]   = {0.28f,0,0.36f,0.05f,0.31f,0.12f,0.28f,0.22f,0.27f,0.60f,0.32f,0.68f,0.35f,0.78f,0.35f,0.88f,0.26f,0.84f,0.26f,0.93f};
    static const float bishop[] = {0.27f,0,0.35f,0.05f,0.29f,0.12f,0.15f,0.22f,0.13f,0.58f,0.20f,0.70f,0.22f,0.78f,0.14f,0.90f,0.175f,0.97f,0.08f,1.08f,0.105f,1.13f,0.0f,1.18f};
    static const float queen[]  = {0.29f,0,0.38f,0.05f,0.31f,0.12f,0.17f,0.22f,0.15f,0.64f,0.23f,0.78f,0.17f,0.86f,0.29f,0.98f,0.23f,1.04f,0.16f,1.14f,0.20f,1.19f,0.0f,1.33f};
    static const float king[]   = {0.30f,0,0.39f,0.05f,0.32f,0.12f,0.18f,0.22f,0.16f,0.70f,0.24f,0.84f,0.18f,0.92f,0.29f,1.04f,0.225f,1.10f,0.17f,1.20f,0.13f,1.28f,0.10f,1.34f};

    for (int c = 0; c < 2; c++) {
        uint16_t lo = c == 0 ? shade(0.88f, 0.86f, 0.76f) : shade(0.26f, 0.22f, 0.26f);   /* ivory / charcoal */
        uint16_t hi = c == 0 ? shade(0.26f, 0.22f, 0.26f) : shade(0.88f, 0.86f, 0.76f);   /* inverted: top finials */

        piece_revolve(&piece_mesh[CHAL_PAWN - 1][c],   pawn,   10, lo, 6);    /* pawns: 16 of them, keep cheap */
        piece_revolve(&piece_mesh[CHAL_BISHOP - 1][c], bishop, 12, lo, 10);
        piece_revolve(&piece_mesh[CHAL_ROOK - 1][c],   rook,   10, lo, 10);   /* top now capped */
        build_knight(&piece_mesh[CHAL_KNIGHT - 1][c], lo);

        /* queen: coronet + ball in the inverted colour */
        piece_revolve(&piece_mesh[CHAL_QUEEN - 1][c], queen, 12, lo, 10);
        for (int k = 0; k < 6; k++) {
            float a = k * TAU / 6;
            piece_add_box(&piece_mesh[CHAL_QUEEN - 1][c],
                          cosf(a) * 0.25f, 1.04f, sinf(a) * 0.25f, 0.05f, 0.09f, 0.05f, hi);
        }
        piece_add_box(&piece_mesh[CHAL_QUEEN - 1][c], 0, 1.20f, 0, 0.10f, 0.10f, 0.10f, hi);   /* coronet ball */
        piece_mesh[CHAL_QUEEN - 1][c].mesh.nverts = piece_mesh[CHAL_QUEEN - 1][c].vert_count;
        piece_mesh[CHAL_QUEEN - 1][c].mesh.nfaces = piece_mesh[CHAL_QUEEN - 1][c].face_count;

        /* king: knob top + cross in the inverted colour */
        piece_revolve(&piece_mesh[CHAL_KING - 1][c], king, 12, lo, 10);
        piece_add_box(&piece_mesh[CHAL_KING - 1][c], 0, 1.46f, 0, 0.05f, 0.14f, 0.05f, hi);   /* vertical bar, base 1.32 on knob */
        piece_add_box(&piece_mesh[CHAL_KING - 1][c], 0, 1.50f, 0, 0.14f, 0.05f, 0.05f, hi);   /* horizontal bar */
        piece_mesh[CHAL_KING - 1][c].mesh.nverts = piece_mesh[CHAL_KING - 1][c].vert_count;
        piece_mesh[CHAL_KING - 1][c].mesh.nfaces = piece_mesh[CHAL_KING - 1][c].face_count;
    }
}

static void build_board(void) {
    for (int r = 0; r <= 8; r++) {
        for (int f = 0; f <= 8; f++) {
            MeshVert *v = &board_verts[r * 9 + f];
            v->x = (int8_t)((f - 4) * 127 / 4);
            v->y = 0;
            v->z = (int8_t)((r - 4) * 127 / 4);
        }
    }

    int nf = 0;
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int a = r * 9 + f, b = a + 1, c = a + 9, d = c + 1;
            uint16_t col = ((r + f) & 1) ? shade(0.20f, 0.42f, 0.28f)
                                         : shade(0.74f, 0.78f, 0.66f);
            board_fcol[nf] = col; board_faces[nf++] = (MeshFace){(uint8_t)a, (uint8_t)c, (uint8_t)b, 0, 127, 0};
            board_fcol[nf] = col; board_faces[nf++] = (MeshFace){(uint8_t)b, (uint8_t)c, (uint8_t)d, 0, 127, 0};
        }
    }

    board_mesh.verts = board_verts;
    board_mesh.faces = board_faces;
    board_mesh.face_colors = board_fcol;
    board_mesh.nverts = 81;
    board_mesh.nfaces = nf;
    board_mesh.scale = 4.0f;
    board_mesh.bound_r = 7.0f;
}

/* Flat square-ring highlight (a box around a square) — cursor + selection. */
static MeshVert cursor_verts[8], select_verts[8];
static MeshFace cursor_faces[8], select_faces[8];
static Mesh     cursor_mesh, select_mesh;

static void build_frame(MeshVert *v, MeshFace *f, Mesh *m, uint16_t col) {
    int8_t outer = (int8_t)(0.47f * 127);
    int8_t inner = (int8_t)(0.36f * 127);
    int8_t y     = (int8_t)(0.03f * 127);

    v[0] = (MeshVert){(int8_t)-outer, y, (int8_t)-outer};
    v[1] = (MeshVert){outer,          y, (int8_t)-outer};
    v[2] = (MeshVert){outer,          y, outer};
    v[3] = (MeshVert){(int8_t)-outer, y, outer};
    v[4] = (MeshVert){(int8_t)-inner, y, (int8_t)-inner};
    v[5] = (MeshVert){inner,          y, (int8_t)-inner};
    v[6] = (MeshVert){inner,          y, inner};
    v[7] = (MeshVert){(int8_t)-inner, y, inner};

    int nf = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        int o0 = i, o1 = j, i0 = 4 + i, i1 = 4 + j;
        f[nf++] = (MeshFace){(uint8_t)o0, (uint8_t)i1, (uint8_t)o1, 0, 127, 0};
        f[nf++] = (MeshFace){(uint8_t)o0, (uint8_t)i0, (uint8_t)i1, 0, 127, 0};
    }

    m->verts = v;
    m->faces = f;
    m->nverts = 8;
    m->nfaces = 8;
    m->color = col;
    m->scale = 1.0f;
    m->bound_r = 0.9f;
}

/* ============================================================
 * Game state
 * ============================================================ */

static void *dyn_buffer, *tt_buffer;

static chal_move_info_t legal_moves[220];
static int legal_move_count;

static int cursor_file = 4, cursor_rank = 6;   /* rank 7 = white back row */
static int has_selection = 0;
static int select_file, select_rank;

enum { ST_PLAYER, ST_ANIM, ST_THINK, ST_SEARCH, ST_OVER };
static int state = ST_PLAYER;

static int result = 0;                 /* 0 none, 1 white mates, 2 black mates, 3 draw */

/* camera orbit */
static float cam_yaw = 0.0f, cam_pitch = 0.85f, cam_dist = 11.0f, gallery_spin = 0.0f;
static int   showing_gallery = 0;

/* move animation */
static float anim_t;
static Vec3  anim_from, anim_to;
static int   anim_type, anim_color, anim_active, anim_to_rank, anim_to_file;

static Vec3 square_world(int rank, int file) {
    return v3((float)(file - 4) + 0.5f, 0.0f, (float)(rank - 4) + 0.5f);
}

/* Convert (rank, file) to the engine's 0x88 square index. */
static uint8_t to_sq88(int rank, int file) {
    return (uint8_t)(((7 - rank) << 4) | file);
}

static void refresh_moves(void) {
    legal_move_count = chal_get_legal_moves(legal_moves, 220);
}

static void check_end(void) {
    if (chal_is_checkmate())      result = (chal_get_side() == CHAL_WHITE) ? 2 : 1;
    else if (chal_is_stalemate()) result = 3;
    if (result) state = ST_OVER;
}

static void start_anim(int from_rank, int from_file, int to_rank, int to_file,
                       int type, int color) {
    anim_from = square_world(from_rank, from_file);
    anim_to = square_world(to_rank, to_file);
    anim_type = type;
    anim_color = color;
    anim_t = 0.0f;
    anim_active = 1;
    anim_to_rank = to_rank;
    anim_to_file = to_file;
}

static void g_init(void) {
    mote->scene_set_background(MOTE_RGB565(36, 42, 60));
    mote->scene_set_sun(v3_norm(v3(0.4f, 1.0f, 0.3f)));

    dyn_buffer = mote->alloc((uint32_t)chal_get_dynamic_size());
    chal_set_dynamic_buffer(dyn_buffer);
    chal_init();

    int tt_count = 256;
    tt_buffer = mote->alloc((uint32_t)(tt_count * chal_get_tt_entry_size()));
    chal_set_tt(tt_buffer, tt_count);
    chal_new_game();

    build_pieces();
    build_board();
    refresh_moves();
    build_frame(cursor_verts, cursor_faces, &cursor_mesh, shade(1.0f, 0.92f, 0.30f));   /* cursor: yellow */
    build_frame(select_verts, select_faces, &select_mesh, shade(0.35f, 0.65f, 1.0f));   /* selection: blue */
}

static Vec3 cam_pos, cam_target;
static Mat3 cam_basis;
static const Mat3 ROT180 = {{ {-1,0,0}, {0,1,0}, {0,0,-1} }};

/* Find a legal move from->to; writes its promotion piece. Returns 1 if found. */
static int find_move(int from_rank, int from_file, int to_rank, int to_file, int *promo) {
    uint8_t from = to_sq88(from_rank, from_file);
    uint8_t to = to_sq88(to_rank, to_file);
    for (int i = 0; i < legal_move_count; i++) {
        if (legal_moves[i].from == from && legal_moves[i].to == to) {
            *promo = legal_moves[i].promo;
            return 1;
        }
    }
    return 0;
}

/* Read the piece on a square; writes type/color. Returns 0 if empty. */
static int piece_at(int rank, int file, int *type, int *color) {
    int p = chal_get_piece(rank, file);
    if (!p) return 0;
    *type = p & 7;
    *color = p >> 3;
    return 1;
}

static void play(int from_rank, int from_file, int to_rank, int to_file, int promo) {
    int type, color;
    piece_at(from_rank, from_file, &type, &color);
    chal_play_move(to_sq88(from_rank, from_file), to_sq88(to_rank, to_file), promo);
    start_anim(from_rank, from_file, to_rank, to_file, type, color);
    refresh_moves();
    state = ST_ANIM;
}

/* Pick the mesh basis for a piece (white knights face the other way). */
static Mat3 piece_basis(int type, int color) {
    return (type == CHAL_KNIGHT && color == CHAL_WHITE) ? ROT180 : m3_identity();
}

static void g_update(float dt) {
    const MoteInput *in = mote->input();

    /* Camera: hold LB + D-pad UP/DOWN = ZOOM; hold RB + D-pad = PAN (LR yaw,
     * UD pitch). Hold BOTH shoulders to peek the piece gallery. */
    int lb = mote_pressed(in, MOTE_BTN_LB);
    int rb = mote_pressed(in, MOTE_BTN_RB);
    int gallery = lb && rb;
    showing_gallery = gallery;

    if (!gallery && lb) {
        if (mote_pressed(in, MOTE_BTN_UP))    cam_dist -= 9.0f * dt;   /* zoom */
        if (mote_pressed(in, MOTE_BTN_DOWN))  cam_dist += 9.0f * dt;
        if (mote_pressed(in, MOTE_BTN_LEFT))  cam_yaw  -= 1.6f * dt;   /* pan L/R */
        if (mote_pressed(in, MOTE_BTN_RIGHT)) cam_yaw  += 1.6f * dt;
    }
    if (!gallery && rb) {
        if (mote_pressed(in, MOTE_BTN_LEFT))  cam_yaw   -= 1.6f * dt;
        if (mote_pressed(in, MOTE_BTN_RIGHT)) cam_yaw   += 1.6f * dt;
        if (mote_pressed(in, MOTE_BTN_UP))    cam_pitch += 1.1f * dt;
        if (mote_pressed(in, MOTE_BTN_DOWN))  cam_pitch -= 1.1f * dt;
    }
    cam_pitch = mote_clampf(cam_pitch, 0.18f, 1.45f);
    cam_dist = mote_clampf(cam_dist, 4.5f, 20.0f);
    gallery_spin += dt * 0.8f;

    if (state == ST_PLAYER && !lb && !rb) {
        /* file L/R is screen-relative: at the default view +x (file 7) is screen-left */
        if (mote_just_pressed(in, MOTE_BTN_LEFT)  && cursor_file < 7) cursor_file++;
        if (mote_just_pressed(in, MOTE_BTN_RIGHT) && cursor_file > 0) cursor_file--;
        if (mote_just_pressed(in, MOTE_BTN_UP)    && cursor_rank > 0) cursor_rank--;
        if (mote_just_pressed(in, MOTE_BTN_DOWN)  && cursor_rank < 7) cursor_rank++;

        if (mote_just_pressed(in, MOTE_BTN_A)) {
            int type, color;
            if (!has_selection) {
                if (piece_at(cursor_rank, cursor_file, &type, &color) && color == CHAL_WHITE) {
                    has_selection = 1;
                    select_file = cursor_file;
                    select_rank = cursor_rank;
                }
            } else {
                int promo;
                if (find_move(select_rank, select_file, cursor_rank, cursor_file, &promo)) {
                    has_selection = 0;
                    play(select_rank, select_file, cursor_rank, cursor_file, promo);
                } else if (piece_at(cursor_rank, cursor_file, &type, &color) && color == CHAL_WHITE) {
                    select_file = cursor_file;
                    select_rank = cursor_rank;
                } else {
                    has_selection = 0;
                }
            }
        }
    }

    if (gallery) {   /* ---- piece gallery ---- */
        cam_pos = v3(0, 1.7f, 5.6f);
        cam_target = v3(0, 0.42f, 0);
        Vec3 fwd = v3_norm(v3_sub(cam_target, cam_pos));
        Vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
        cam_basis.r[0] = right;
        cam_basis.r[1] = v3_cross(fwd, right);
        cam_basis.r[2] = fwd;
        mote->scene_camera(&cam_basis, cam_pos, 62.0f);

        Mat3 spin;
        float cs = cosf(gallery_spin), sn = sinf(gallery_spin);
        spin.r[0] = v3(cs, 0, sn);
        spin.r[1] = v3(0, 1, 0);
        spin.r[2] = v3(-sn, 0, cs);

        int order[6] = {CHAL_PAWN, CHAL_KNIGHT, CHAL_BISHOP, CHAL_ROOK, CHAL_QUEEN, CHAL_KING};
        for (int i = 0; i < 6; i++) {
            Vec3 pos = v3((i - 2.5f) * 1.28f, 0, 0);
            mote_draw_ex(mote, &piece_mesh[order[i] - 1][0].mesh, pos, spin, 1.0f);
        }
        return;
    }

    if (state == ST_ANIM) {
        anim_t += dt / 0.45f;
        if (anim_t >= 1.0f) {
            anim_active = 0;
            check_end();
            if (state != ST_OVER)
                state = (anim_color == CHAL_WHITE) ? ST_THINK : ST_PLAYER;
        }
    } else if (state == ST_THINK) {
        state = ST_SEARCH;            /* render one "thinking" frame first */
    } else if (state == ST_SEARCH) {
        chal_move_info_t m = chal_search_best_move(6, 700);
        if (m.from == 0x80) {
            check_end();
            state = ST_OVER;
        } else {
            int to_rank = 7 - (m.to >> 4), to_file = m.to & 7;
            int from_rank = 7 - (m.from >> 4), from_file = m.from & 7;
            play(from_rank, from_file, to_rank, to_file, m.promo);
        }
    }

    /* ---- camera (orbit around the board centre) ---- */
    cam_target = v3(0, 0.3f, 0);
    cam_pos = v3(cam_target.x + sinf(cam_yaw) * cosf(cam_pitch) * cam_dist,
                 cam_target.y + sinf(cam_pitch) * cam_dist,
                 cam_target.z + cosf(cam_yaw) * cosf(cam_pitch) * cam_dist);
    cam_basis = mote_camera_look(cam_pos, cam_target);

    mote->scene_camera(&cam_basis, cam_pos, 58.0f);

    /* board */
    mote_draw(mote, &board_mesh, v3(0, 0, 0));

    /* settled pieces (skip the one currently animating to its destination) */
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            if (anim_active && r == anim_to_rank && f == anim_to_file) continue;
            int type, color;
            if (!piece_at(r, f, &type, &color)) continue;
            mote_draw_ex(mote, &piece_mesh[type - 1][color].mesh,
                         square_world(r, f), piece_basis(type, color), 1.0f);
        }
    }

    /* animating piece: lift in an arc and glide to its destination */
    if (anim_active) {
        float t = anim_t;
        float lift = sinf(t * 3.14159f) * 0.6f;
        Vec3 pos = v3(anim_from.x + (anim_to.x - anim_from.x) * t,
                      anim_from.y + lift,
                      anim_from.z + (anim_to.z - anim_from.z) * t);
        mote_draw_ex(mote, &piece_mesh[anim_type - 1][anim_color].mesh,
                     pos, piece_basis(anim_type, anim_color), 1.0f);
    }

    /* cursor + selection markers */
    if (state == ST_PLAYER) {
        mote_draw(mote, &cursor_mesh, square_world(cursor_rank, cursor_file));
        if (has_selection)
            mote_draw(mote, &select_mesh, square_world(select_rank, select_file));
    }
}

static void g_overlay(uint16_t *fb) {
    if (state == ST_OVER) {
        const char *msg = result == 1 ? "WHITE WINS" : result == 2 ? "BLACK WINS" : "DRAW";
        mote->text(fb, msg, 38, 58, MOTE_RGB565(255, 240, 120));
        return;
    }
    if (showing_gallery) {
        mote->text(fb, "PIECE GALLERY", 30, 4, MOTE_RGB565(230, 220, 160));
        mote->text(fb, "PAWN KNT BSH ROOK Q K", 2, 118, MOTE_RGB565(200, 210, 225));
        return;
    }
    if (state == ST_THINK || state == ST_SEARCH)
        mote->text(fb, "BLACK THINKING", 26, 4, MOTE_RGB565(230, 180, 120));
    else
        mote->text(fb, has_selection ? "A DROP  LB ZOOM RB PAN" : "A PICK  LB ZOOM RB PAN",
                   4, 4, MOTE_RGB565(210, 220, 235));
    if (chal_is_in_check() && state == ST_PLAYER)
        mote->text(fb, "CHECK", 48, 118, MOTE_RGB565(255, 90, 90));
}

static const MoteGameVtbl k_vtbl = {
    .init = g_init, .update = g_update, .overlay = g_overlay,
    .config = { .max_tris = 5100, .max_spheres = 8, .depth = 1 },   /* no physics, no splats */
};
static const MoteGameVtbl *mote_game_vtbl(void) { return &k_vtbl; }
