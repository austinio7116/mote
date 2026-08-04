/*
 * CueVR — glTF 2.0 binary (.glb), enough of it to draw a controller.
 *
 * XR_FB_render_model hands you the runtime's own model of the hardware the
 * player is actually holding — the right Touch, the Touch Pro, whatever they
 * have — already posed in the controller's grip space. That last part is the
 * reason to do this at all: nine attempts went into guessing the transform from
 * a static STL's own axes onto the grip frame, and the runtime simply knows.
 *
 * What arrives is a GLB, so this parses one. Deliberately a SUBSET, matching
 * what the extension promises (glTF 2.0 subset 1): triangles, indexed, with
 * POSITION / NORMAL / TEXCOORD_0, a node hierarchy of TRS or matrix transforms,
 * and one base-colour texture per material as an embedded PNG. No animation, no
 * skinning, no sparse accessors, no morph targets, no external URIs — a
 * controller model has none of those, and supporting them would be code that
 * has never once run.
 *
 * Everything is flattened at load: node transforms are baked into the vertices,
 * so a drawn model is a plain vertex buffer per material and nothing has to
 * walk a scene graph at 72 Hz.
 */
#ifndef CUEVR_GLB_H
#define CUEVR_GLB_H

#include <stdint.h>

#define CUEVR_GLB_MAX_PARTS 8

typedef struct {
    float p[3];
    float n[3];
    float uv[2];
} CueVrGlbVtx;

/* One material's worth of triangles. A controller is usually one or two of
 * these — a body and a lit ring. */
typedef struct {
    int      first_index;    /* into the model's index array */
    int      n_index;
    int      tex;            /* index into CueVrGlbModel::tex, or -1 */
    float    base[4];        /* baseColorFactor, multiplying the texture */
} CueVrGlbPart;

typedef struct {
    uint8_t *px;             /* RGBA8, top row first */
    int      w, h;
} CueVrGlbTex;

typedef struct {
    CueVrGlbVtx  *v;
    uint32_t     *idx;
    int           nv, ni;

    CueVrGlbPart  part[CUEVR_GLB_MAX_PARTS];
    int           nparts;

    CueVrGlbTex   tex[CUEVR_GLB_MAX_PARTS];
    int           ntex;
} CueVrGlbModel;

/* Parse `bytes`. Returns 0 on success and fills `out`; anything else means the
 * model could not be read and `out` is left zeroed, which the caller should
 * treat as "no render model" and fall back. Never partially fills.
 *
 * The caller owns the result: cuevr_glb_free when done. */
int  cuevr_glb_parse(const void *bytes, uint32_t len, CueVrGlbModel *out);
void cuevr_glb_free(CueVrGlbModel *m);

/* Why the last parse failed, for the log. Static string, never NULL. */
const char *cuevr_glb_error(void);

#endif /* CUEVR_GLB_H */
