/*
 * CueVR — drawing a real table in a real room.
 *
 * GLES 3.0, one program with a mode uniform, and colour handled exactly as the
 * console app handles it: everything lives in linear from sample to submit, and
 * whether the final encode happens here or in the swapchain is asked, not
 * assumed (see mote_xr_target_is_srgb).
 *
 * The table is not modelled here at all. cue_render.c already builds it — the
 * cloth bed fanned over the real knuckle boundary so pocket mouths are true
 * gaps, the K66 cushion cross-section with its overhanging nose and undercut,
 * the splayed jaw facings, the pocket voids, the drop lips, the baulk line and
 * the D — and those shapes, tuned table by table, ARE the game. So CueVR calls
 * cue_render_build_table() and uploads the triangles it produces straight to
 * GL. Same geometry the handheld draws, on all seven tables; the only thing
 * that changes is who rasterises it.
 *
 * Balls are one unit sphere drawn many times, each with its own CueBall.orient
 * — the physics integrates a real orientation matrix from the angular velocity,
 * so screw, follow and side are all visible on the ball as it runs. Stripes and
 * spots are computed in object space in the fragment shader, which means they
 * roll correctly and cost no texture at all.
 */
#include "cuevr.h"
#include "cuevr_render.h"
#include "cuevr_text.h"
#include "cuevr_font_lg.h"
#include "cuevr_font_xl.h"
#include "cuevr_ctrl_left.h"
#include "cuevr_ctrl_right.h"
#include "cue_render.h"
#include "cuevr_frame.h"
#include "craft_font.h"

#include <GLES3/gl3.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "cuevr", __VA_ARGS__)
#else
#define LOGI(...) do { printf(__VA_ARGS__); printf("\n"); } while (0)
#endif

#define PI 3.14159265358979f

/* ---- shaders ------------------------------------------------------------ */

static const char *VS =
"layout(location=0) in vec3 a_pos;\n"
"layout(location=1) in vec3 a_nrm;\n"
"layout(location=2) in vec2 a_uv;\n"
"layout(location=3) in vec3 a_col;\n"
"uniform mat4 u_mvp[2];\n"
"uniform mat4 u_model;\n"
"uniform float u_shell;\n"   // metres to extrude along the normal (fur shells)

"out vec3 v_nrm;\n"
"out vec2 v_uv;\n"
"out vec3 v_local;\n"
"out vec3 v_col;\n"
"out vec3 v_world;\n"
"uniform vec3 u_eye[2];\n"
"out vec3 v_eyepos;\n"
"void main() {\n"

"    v_eyepos = u_eye[VIEW_ID];\n"
"    v_uv = a_uv;\n"
"    v_col = a_col;\n"
"    v_local = a_pos;\n"
"    vec3 p = a_pos + a_nrm * u_shell;\n"
"    v_local = p;\n"
"    v_world = (u_model * vec4(p, 1.0)).xyz;\n"
"    v_nrm = normalize(mat3(u_model) * a_nrm);\n"
"    gl_Position = u_mvp[VIEW_ID] * vec4(p, 1.0);\n"
"}\n";

/* u_mode: 0 lit flat colour, 1 ball, 2 unlit textured (HUD), 3 room grid */
static const char *FS =
"precision highp float;\n"
"in vec3 v_nrm;\n"
"in vec2 v_uv;\n"
"in vec3 v_local;\n"
"in vec3 v_col;\n"
"in vec3 v_world;\n"
"uniform sampler2D u_tex;\n"
"uniform int   u_mode;\n"
"uniform int   u_encode;\n"
"uniform vec4  u_colour;\n"
"uniform vec4  u_colour2;\n"   /* balls: the stripe/secondary colour */
"uniform vec3  u_light;\n"
"uniform vec3  u_lampC[4];\n"   // lamp centres, world space
"uniform vec3  u_lampX[4];\n"   // half-extent along the table's length
"uniform vec3  u_lampZ[4];\n"   // half-extent across it
"uniform int   u_nlamp;\n"
"in vec3 v_eyepos;\n"
"uniform vec3  u_clothsh;\n"    // cloth bounce tint
"uniform vec3  u_cloth;\n"      // the cloth's own colour
"uniform highp sampler2DArray u_fur;\n"
"uniform highp sampler2D u_nap;\n"
"uniform float u_feltspan;\n"
"uniform float u_furslice;\n"
"uniform float u_furslices;\n"
"uniform float u_furdbg;\n"
"uniform vec3  u_cshaft;\n"
"uniform vec3  u_csplice;\n"
"uniform vec3  u_caccent;\n"
"uniform vec3  u_cbutt;\n"
"uniform vec3  u_cburr;\n"
"uniform float u_cflash;\n"
"uniform vec3  u_markc;\n"      // chalk
"uniform float u_baulk;\n"
"uniform float u_drad;\n"
"uniform float u_linew;\n"
"uniform float u_spotr;\n"
"uniform int   u_nspot;\n"
"uniform vec2  u_spots[8];\n"

"uniform float u_ballslice;\n"  /* which layer of the ball array */
/* GLSL ES has no default precision for sampler2DArray — unlike sampler2D —
 * so it must be stated or the shader will not compile at all. */
"uniform highp sampler2DArray u_balls;\n"
"out vec4 o_col;\n"
"vec3 to_linear(vec3 c) {\n"
"    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
"}\n"
"vec3 to_srgb(vec3 c) {\n"
"    c = max(c, 0.0);\n"
"    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0/2.4)) - 0.055, step(0.0031308, c));\n"
"}\n"
"float hash12(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
"// Smoothed value noise. floor()'d hash alone gives square cells, which on cloth\n"
"// reads as digital dirt rather than fibre.\n"
"float vnoise(vec2 p) {\n"
"    vec2 i = floor(p), f = fract(p);\n"
"    f = f * f * (3.0 - 2.0 * f);\n"
"    float a = hash12(i), b = hash12(i + vec2(1.0, 0.0));\n"
"    float c = hash12(i + vec2(0.0, 1.0)), d = hash12(i + vec2(1.0, 1.0));\n"
"    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);\n"
"}\n"
"vec4 emit(vec3 c, float a, float dither) {\n"
"    vec3 o = u_encode == 1 ? to_srgb(c) : c;\n"
"    return vec4(o + (hash12(gl_FragCoord.xy) - 0.5) * dither / 255.0, a);\n"
"}\n"
"// The chalk, as a distance field. Shared by the cloth AND by every shell of the\n"
"// pile, which is the point: chalk is painted ON a cloth, so the pile has to\n"
"// carry it. Drawing it only on the backing meant eight layers of green were\n"
"// then drawn over the top and the baulk line faded out wherever the pile was\n"
"// densest — which is everywhere you are close enough to see the pile at all.\n"
"// ---- cloth sheen: the Charlie distribution -------------------------------\n"
"//\n"
"// Estevez and Kulla, Production Friendly Microfacet Sheen BRDF (SIGGRAPH 2017),\n"
"// as specified in glTF KHR_materials_sheen and used by Autodesk Standard Surface\n"
"// and the UE/HDRP cloth materials. Velvet is a forest of near-vertical specular\n"
"// fibres, so its microfacet distribution peaks at GRAZING half-angles rather than\n"
"// at the normal, an exponentiated sinusoid instead of a Gaussian. That is why the\n"
"// fuzz lights up at the edges and along a shaded slope.\n"
"float sheen_charlie(float NdotH, float rough) {\n"
"    float alpha = max(rough * rough, 1e-4);\n"
"    float invr  = 1.0 / alpha;\n"
"    float sin2  = max(1.0 - NdotH * NdotH, 0.0);\n"
"    return (2.0 + invr) * pow(sin2, invr * 0.5) / 6.2831853;\n"
"}\n"
"// Ashikhmin-Premoze visibility, the simplified option the spec offers. The full\n"
"// Charlie lambda wants a fitted table and this is a mobile GPU.\n"
"float sheen_vis(float NdotL, float NdotV) {\n"
"    return 1.0 / max(4.0 * (NdotL + NdotV - NdotL * NdotV), 1e-4);\n"
"}\n"
"// ---- the nap, as a perturbed NORMAL --------------------------------------\n"
"//\n"
"// Not as brightness. Modulating colour with noise can only ever look like dirt —\n"
"// and with a coverage mask as the source it looked exactly like black speckle over\n"
"// flat green, which is what it was.\n"
"//\n"
"// A real pile leans, and because the sheen lobe peaks at grazing angles a small\n"
"// change of lean swings the brightness a long way. That is where velvet gets its\n"
"// soft sweeping tone. So the field is a signed 2-vector — the direction the pile\n"
"// leans — it bends the shading normal, and the BRDF does the rest.\n"
"//\n"
"// Triplanar, blended by the normal: choosing one axis by whichever component is\n"
"// largest flips partway along a curved cushion and stretches into bands on a\n"
"// slope.\n"
"// A tangent frame built from the normal, so the cloth is sampled on the surface\n"
"// it is actually on.\n"
"//\n"
"// This replaces a triplanar blend, which was wrong here for two reasons. Blending\n"
"// three world-axis projections means the WEIGHTS shift as the normal turns along a\n"
"// curved cushion, so the pattern slides between projections and bands; and the\n"
"// blended result has no single coherent footprint, so derivative-based mip\n"
"// selection takes a coarse level along one axis and smears it into stretched\n"
"// streaks — which is exactly what the cushion edges were doing.\n"
"//\n"
"// One projection onto the surface plane has a true footprint, so anisotropic\n"
"// filtering works on it, and the frame rotates smoothly with the normal instead of\n"
"// switching. Standard way to texture a smooth surface that has no UVs.\n"
"vec2 surf_uv(vec3 pos, vec3 nrm) {\n"
"    vec3 n = normalize(nrm);\n"
"    vec3 up = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
"    vec3 t = normalize(cross(up, n));\n"
"    vec3 b = cross(n, t);\n"
"    return vec2(dot(pos, t), dot(pos, b));\n"
"}\n"
"// ---- the nap, in TWO texture fetches -------------------------------------\n"
"//\n"
"// It was ten. cloth_normal took two nap_lean calls, lean_lost another and\n"
"// nap_occlusion two more, each of those doing two textureGrad with its own\n"
"// derivative maths — over most of the screen at 90 Hz on a mobile GPU. That is\n"
"// what collapsed the frame rate, and it was pure waste: every one of those\n"
"// consumers wants the same field, just at two scales.\n"
"//\n"
"// So: sample once coarse, once fine, and share. The lean, the occlusion and the\n"
"// Toksvig roughness all come out of the same two lookups.\n"
"\n"
"// A cushion top seen almost edge-on has a footprint tens of times longer along the\n"
"// strip than across it, so automatic LOD takes the longer axis and blurs it to\n"
"// mush. Hardware aniso stops at 16:1. Shortening the longer gradient until the\n"
"// ratio is inside that hands the GPU something it can actually filter.\n"
"vec2 nap_fetch(vec2 uv) {\n"
"    vec2 dx = dFdx(uv), dy = dFdy(uv);\n"
"    float lx = max(length(dx), 1e-8), ly = max(length(dy), 1e-8);\n"
"    float cap = min(lx, ly) * 12.0;\n"
"    if (lx > cap) dx *= cap / lx;\n"
"    if (ly > cap) dy *= cap / ly;\n"
"    return (textureGrad(u_nap, uv, dx, dy).rg - 0.5) * 2.0;\n"
"}\n"
"\n"
"struct NapSample { vec2 coarse; vec2 fine; };\n"
"\n"
"NapSample nap_sample(vec3 pos, vec3 nrm) {\n"
"    vec2 uv = surf_uv(pos, nrm);\n"
"    NapSample s;\n"
"    s.coarse = nap_fetch(uv / 0.045);\n"
"    // The fine one takes a plain fetch: it feeds occlusion, where a slightly soft\n"
"    // grazing footprint costs nothing anyone can see, and it saves the derivative\n"
"    // work and the branches.\n"
"    s.fine = (texture(u_nap, uv / 0.0085).rg - 0.5) * 2.0;\n"
"    return s;\n"
"}\n"
"\n"
"vec3 cloth_normal(vec3 nrm, NapSample s) {\n"
"    vec2 lean = s.coarse * 0.36 + s.fine * 0.13;\n"
"    vec3 n = normalize(nrm);\n"
"    vec3 up = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
"    vec3 t = normalize(cross(up, n));\n"
"    vec3 b = cross(n, t);\n"
"    return normalize(n + t * lean.x + b * lean.y);\n"
"}\n"
"\n"
"// Micro-occlusion between fibres: where the pile is tilted or parted its gaps are\n"
"// shadowed. Light-INDEPENDENT, which is why it works where sheen cannot — sheen is\n"
"// grazing-weighted by construction, so a surface lit square on gets none of it.\n"
"float nap_occlusion(NapSample s) {\n"
"    float cav = length(s.fine) * 0.65 + length(s.coarse) * 0.35;\n"
"    return 1.0 - clamp(cav * 0.38, 0.0, 0.14);\n"
"}\n"
"\n"
"// Toksvig 2005: mipmapping a NORMAL field averages the vectors toward zero, so fine\n"
"// detail vanishes rather than softening. Widen the lobe by however much was eaten.\n"
"float lean_lost(NapSample s) {\n"
"    return clamp(1.0 - length(s.coarse) / 0.44, 0.0, 1.0);\n"
"}\n"
"\n"
"// ---- timber ---------------------------------------------------------------\n"
"//\n"
"// A plank is a slice through a log, so its figure is GROWTH RINGS cut at an angle\n"
"// — not stripes. That is the whole difference between wood and corduroy, and both\n"
"// of my earlier attempts drew stripes: one loud, one quiet, neither wood.\n"
"//\n"
"// The model, in the order it matters:\n"
"//\n"
"//  1. Distance from the PITH. The pith is the centre of the log, a line running\n"
"//     parallel to the plank and offset outside it, so rings sweep across the board\n"
"//     rather than circling on it. How far off-centre decides whether the board\n"
"//     reads as flat-sawn (broad cathedral figure) or quarter-sawn (tight parallel\n"
"//     lines) — one constant covers the whole range.\n"
"//  2. TURBULENCE on that distance before ringing it. Real rings wander; perfectly\n"
"//     concentric ones are a machined part. This is what stops it being a barcode.\n"
"//  3. An ASYMMETRIC ring profile. Earlywood is wide, pale and soft; latewood is a\n"
"//     narrow, dark, hard band at the end of each season. A sine gives none of that\n"
"//     — the asymmetry is most of what your eye reads as timber.\n"
"//  4. Ray fleck across the grain, the medullary rays that catch light on quartered\n"
"//     stock.\n"
"//  5. Open pores: fine dark flecks drawn out ALONG the grain, which is what walnut\n"
"//     and oak have and what pine does not.\n"
"//\n"
"// Two fetches, the same budget the stripes cost.\n"
"vec3 timber(vec3 base, vec2 wq, vec2 warp, vec2 fine, vec3 nrm, vec3 Ldir,\n"
"            out float varnish) {\n"
"    const float PITH_OFF   = 0.085;   /* metres from the board to the log centre */\n"
"    const float RINGS_PER_M = 105.0;  /* ~9.5 mm a season, as a hardwood rail */\n"
"    const float RING = 1.0 / RINGS_PER_M;   /* one ring, in metres */\n"
"    // EVERY perturbation below is a fraction of RING. Expressed in absolute\n"
"    // metres they were enormous next to it — the first attempt displaced the pith\n"
"    // distance by up to 30 mm on a 4.8 mm ring, which is six rings of shift per\n"
"    // pixel, so the rings scrambled into speckle and it came out looking like cork.\n"
"\n"
"    // 1 + 2: warped distance from the pith line.\n"
"    float across = wq.y + PITH_OFF;\n"
"    float along  = wq.x;\n"
"    // A gentle bend across the board: a couple of rings over its whole width.\n"
"    float d = length(vec2(across + warp.x * RING * 1.6, 0.020 + warp.y * RING * 0.8));\n"
"    // And a wander in the ring EDGE of about a fifth of a ring, which is what stops\n"
"    // it being a machined part without destroying the rings themselves.\n"
"    d += warp.y * RING * 0.20 + fine.y * RING * 0.06;\n"
"\n"
"    // 3: the ring, asymmetric. late is a narrow dark band; the rest eases.\n"
"    float r = fract(d * RINGS_PER_M);\n"
"    float late = smoothstep(0.62, 0.80, r) * (1.0 - smoothstep(0.86, 0.99, r));\n"
"    float early = smoothstep(0.10, 0.55, r);\n"
"\n"
"    // 4: ray fleck, short bright dashes ACROSS the grain on quartered stock.\n"
"    float fleck = smoothstep(0.86, 0.99, fine.x + 0.5)\n"
"                * (1.0 - smoothstep(0.45, 0.85, abs(fract(along * 26.0) - 0.5) * 2.0));\n"
"\n"
"    // 5: pores, drawn out along the grain.\n"
"    float pore = smoothstep(0.93, 0.995, fine.y + 0.5);\n"
"\n"
"    // Colour. Latewood is darker AND more saturated, which is the part that makes\n"
"    // it look like a material rather than a greyscale pattern laid over a tint.\n"
"    vec3 c = base * (0.93 + 0.14 * early);\n"
"    vec3 lw = base * 0.52;\n"
"    lw.r *= 1.10; lw.b *= 0.88;                  /* warmer in the dark bands */\n"
"    // Gently. A rail is furniture: the figure should be legible when you look at\n"
"    // it and invisible when you are looking at the table. At 0.85 the bands were\n"
"    // woodgrain wallpaper.\n"
"    c = mix(c, lw, late * 0.40);\n"
"    c += base * fleck * 0.10;\n"
"    c *= 1.0 - pore * 0.14;\n"
"    // VARNISH, and it is anisotropic. This is most of what separates polished timber\n"
"    // from a brown pattern: the highlight on a finished rail stretches ALONG the\n"
"    // grain, because the fibres under the finish are cylinders lying in one\n"
"    // direction. Same Kajiya-Kay lobe as the cloth, tighter, tangent along the board.\n"
"    //\n"
"    // Returned SEPARATELY rather than added into the colour here. The caller\n"
"    // multiplies the diffuse by N.L, and a specular folded in before that gets\n"
"    // multiplied by it too — which is not how light works and is why the sheen was\n"
"    // only showing where the surface was dark.\n"
"    //\n"
"    // Brighter over latewood: the dense bands take a finish harder than the soft\n"
"    // earlywood, so the sheen carries the ring pattern. That coupling is what makes\n"
"    // it one material instead of a gloss layer on a texture.\n"
"    {\n"
"        vec3 n = normalize(nrm);\n"
"        vec3 up = abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);\n"
"        vec3 tg = normalize(cross(up, n));      /* along the board, as surf_uv */\n"
"        vec3 Vv = normalize(v_eyepos - v_world);\n"
"        float TdL = dot(tg, Ldir), TdV = dot(tg, Vv);\n"
"        float sL = sqrt(max(1.0 - TdL * TdL, 0.0));\n"
"        float sV = sqrt(max(1.0 - TdV * TdV, 0.0));\n"
"        // 120 was a razor line that only lit where the geometry happened to satisfy\n"
"        // it. 34 is a sheen you can see travel as you move.\n"
"        float aniso = pow(max(sL * sV - TdL * TdV, 0.0), 34.0);\n"
"        float broad = pow(max(dot(n, normalize(Ldir + Vv)), 0.0), 16.0);\n"
"        // Weighted by N.L, because a specular still needs the light to REACH the\n"
"        // surface. Without it the anisotropic lobe blew the inside of the pocket\n"
"        // throat out to white — a surface facing away from the lamp was returning\n"
"        // a full highlight. And the amplitude was far too high for a rail.\n"
"        float lit = max(dot(n, Ldir), 0.0);\n"
"        varnish = (aniso * (0.30 + 0.70 * late) * 0.34 + broad * 0.06) * lit;\n"
"    }\n"
"    return c;\n"
"}\n"
"\n"
"float mark_cov(vec2 q, float aa) {\n"

"    float m = 0.0;\n"
"    if (u_linew > 0.0) {\n"
"        float dl = abs(q.x - u_baulk);\n"
"        m = 1.0 - smoothstep(u_linew, u_linew + aa, dl);\n"
"        if (u_drad > 0.0 && q.x < u_baulk) {\n"
"            float dr = abs(length(q - vec2(u_baulk, 0.0)) - u_drad);\n"
"            m = max(m, 1.0 - smoothstep(u_linew, u_linew + aa, dr));\n"
"        }\n"
"    }\n"
"    for (int i = 0; i < 8; i++) {\n"
"        if (i >= u_nspot) break;\n"
"        float d = length(q - u_spots[i]);\n"
"        m = max(m, 1.0 - smoothstep(u_spotr * 0.72, u_spotr + aa, d));\n"
"    }\n"
"    float g1 = vnoise(q * 1600.0);\n"
"    float g2 = vnoise(q * 420.0 + 3.1);\n"
"    return clamp(m * (0.58 + 0.44 * g1 + 0.22 * g2), 0.0, 1.0);\n"
"}\n"
"void main() {\n"

"    vec3 L = normalize(u_light);\n"
"    if (u_mode == 2) {\n"
"        vec4 t = texture(u_tex, v_uv);\n"
"        o_col = emit(to_linear(t.rgb), t.a * u_colour.a, 0.0);\n"
"    } else if (u_mode == 3) {\n"
"        vec2 g = abs(fract(v_local.xz * 2.0) - 0.5) / fwidth(v_local.xz * 2.0);\n"
"        float line = 1.0 - min(min(g.x, g.y), 1.0);\n"
"        float fade = clamp(1.0 - length(v_local.xz) / 7.0, 0.0, 1.0);\n"
"        o_col = emit(to_linear(u_colour.rgb) * line * fade, line * fade, 1.0);\n"
"    } else if (u_mode == 1) {\n"
"        // A ball, shaded the way the handheld shades it — this is\n"
"        // cue_ball_shade()'s default mode, not an approximation of it. The\n"
"        // albedo is ball_sample() baked into an atlas and looked up by the\n"
"        // BALL-LOCAL normal, so the numbers turn with the ball; the lighting\n"
"        // on top is the snooker rig: one nearly-overhead key, the cloth\n"
"        // bouncing colour back up onto the underside, and four lamps whose\n"
"        // sharp reflections are what make a polished ball look polished.\n"
"        // All of it in the same non-linear space shade565() worked in, or the\n"
"        // balance between the three comes out wrong.\n"
"        vec3 nb = normalize(v_local);\n"
"        // Longitude runs the other way. The handheld's sphere impostor builds\n"
"        // its normal as (u, -v, -nz) — a MIRROR of the true outward normal in\n"
"        // the view's forward axis, not a rotation of it — so ball_sample() has\n"
"        // always been fed a left-handed normal and its numbers are painted for\n"
"        // one. Baking from a true outward normal therefore comes out mirrored.\n"
"        // Flipping longitude here mirrors it back, and leaves latitude and the\n"
"        // lighting normal alone.\n"
"        float u = 0.5 - atan(nb.z, nb.x) / 6.2831853;\n"
"        float vv = acos(clamp(nb.y, -1.0, 1.0)) / 3.14159265;\n"
"        // A texture ARRAY, not one tall atlas. An atlas cannot be mipmapped:\n"
"        // level 1 blends the bottom of one ball into the top of the next, so\n"
"        // the choice would be a sharp ball that shimmers or a smooth ball with\n"
"        // its neighbour bleeding into its poles. A layer per ball has its own\n"
"        // mip chain and has neither problem.\n"
"        // The seam needs textureGrad, not texture().\n"
"        //\n"
"        // atan() jumps from +PI to -PI once round the ball, so u jumps by a\n"
"        // whole 1.0 across a single pixel column. GL_REPEAT samples that\n"
"        // correctly, but the implicit DERIVATIVE does not care about wrapping:\n"
"        // it sees a full-texture step in one pixel, concludes the footprint is\n"
"        // the entire map, and picks the coarsest mip available. That is the\n"
"        // narrow disconnected line running pole to pole — a meridian, which on a\n"
"        // belt-striped ball cuts across the stripe at right angles.\n"
"        //\n"
"        // Subtracting round() removes the spurious jump and leaves the real\n"
"        // rate of change, so the seam picks the same mip as its neighbours.\n"
"        vec2 gu = vec2(dFdx(u), dFdy(u));\n"
"        gu -= round(gu);\n"
"        vec2 gv = vec2(dFdx(vv), dFdy(vv));\n"
"        vec3 bc = textureGrad(u_balls, vec3(u, clamp(vv, 0.001, 0.999), u_ballslice),\n"
"                              vec2(gu.x, gv.x), vec2(gu.y, gv.y)).rgb;\n"
"        vec3 nw = normalize(v_nrm);\n"
"        float diff = max(dot(nw, L), 0.0);\n"
"        float down = max(-nw.y, 0.0);\n"
"        // Diffuse, then the cloth's bounce ADDED rather than mixed in.\n"
"        // The handheld lerps up to 82% toward the cloth tint on the\n"
"        // shadow side, which at 128x128 reads as 'in shadow' and at this\n"
"        // size reads as a red ball turning muddy green. Adding the bounce\n"
"        // instead lights the underside without draining the hue out of it,\n"
"        // and a snooker ball under a lamp is a *saturated* object.\n"
"        vec3 c = bc * (0.52 + 0.62 * diff) + u_clothsh * (down * 0.34 + 0.10);\n"
"        // The lamps, reflected properly.\n"
"        //\n"
"        // The handheld tests the half-vector against 0.975 and lights a\n"
"        // pixel or two. That IS the right answer at 128x128 — a single white\n"
"        // dot is all the room there is to say 'lamp'. Here a ball is two\n"
"        // hundred pixels across and the same test renders as nothing at all,\n"
"        // so this does what a polished ball actually does: mirror the shade.\n"
"        // Reflect the view vector about the surface, intersect the reflected\n"
"        // ray with each lamp rectangle hanging over the table, and light the\n"
"        // fragment where it hits one. The highlights are then the shape of\n"
"        // the lamps, they stretch and skew across the curve the way real\n"
"        // ones do, and they slide correctly as you walk around the table —\n"
"        // none of which a half-vector threshold can do.\n"
"        vec3 V = normalize(v_eyepos - v_world);\n"
"        vec3 Rv = reflect(-V, nw);\n"
"        float refl = 0.0;\n"
"        if (Rv.y > 1e-4) {\n"
"            for (int i = 0; i < u_nlamp; i++) {\n"
"                float t = (u_lampC[i].y - v_world.y) / Rv.y;\n"
"                if (t <= 0.0) continue;\n"
"                vec3 d = (v_world + Rv * t) - u_lampC[i];\n"
"                float a = dot(d, u_lampX[i]) / dot(u_lampX[i], u_lampX[i]);\n"
"                float b = dot(d, u_lampZ[i]) / dot(u_lampZ[i], u_lampZ[i]);\n"
"                // A shade has a hard edge and a hot centre. smoothstep over\n"
"                // the last few percent keeps it from aliasing to a crawling\n"
"                // staircase as the ball rolls.\n"
"                float e = max(abs(a), abs(b));\n"
"                refl += (1.0 - smoothstep(0.88, 1.0, e)) * (1.0 - 0.25 * e);\n"
"            }\n"
"        }\n"
"        // The shade is a bright source: let it blow out to white.\n"
"        c += vec3(1.0) * clamp(refl, 0.0, 1.4);\n"
"        o_col = emit(to_linear(c), 1.0, 1.0);\n"
"    } else if (u_mode == 5) {\n"
"        // The cue. v_uv.y is the fraction along it (0 = tip) and v_uv.x runs\n"
"        // around it, which is all a hand-spliced cue needs: leather, ferrule,\n"
"        // ash, four ebony points and an ebony butt, with grain along the\n"
"        // shaft. No texture, and it stays sharp with the tip a hand's width\n"
"        // from your eye.\n"
"        float t = v_uv.y, a = v_uv.x;\n"
"        vec3 ash   = u_cshaft;\n"
"        vec3 ebony = u_csplice;\n"
"        vec3 c;\n"
"        float gloss = 42.0, spec_k = 0.30;\n"
"        // A snooker tip is GREEN leather and the ferrule is bright metal. It\n"
"        // had them the other way about, a blue-grey pad behind a green band,\n"
"        // because the ferrule was doubling as the aim indicator. That is now\n"
"        // a brightening of the metal, so the tip can be the colour it is.\n"
"        // 3.5 mm of leather then a 10 mm ferrule, as measured lengths and not\n"
"        // as guesses: the green ran to 11 mm before, which is three times the\n"
"        // pad on a real cue and read as a long coloured cone.\n"
"        if (t < 0.00241) { c = vec3(0.16, 0.42, 0.30); gloss = 6.0; spec_k = 0.04; }\n"
"        else if (t < 0.00931) {\n"
"            // A ferrule is polished metal, and flat grey with one Blinn\n"
"            // highlight is what plastic looks like. Metal has almost no\n"
"            // diffuse and nearly all of its brightness comes from what it is\n"
"            // reflecting, so it needs to go DARK where it reflects nothing and\n"
"            // blow out where it catches a lamp. The band round the shaft is\n"
"            // the giveaway: bright top, dark flank, bright rim.\n"
"            vec3 nn = normalize(v_nrm);\n"
"            vec3 Vv = normalize(v_eyepos - v_world);\n"
"            vec3 R = reflect(-Vv, nn);\n"
"            float up = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);\n"
"            float band = pow(up, 3.0);\n"
"            float rim = pow(1.0 - abs(dot(nn, Vv)), 2.5);\n"
"            vec3 metal = vec3(0.62, 0.63, 0.66);\n"
"            c = metal * (0.42 + 1.25 * band) + vec3(rim * 0.45);\n"
"            // a tight lamp glint on top of that\n"
"            float g = pow(max(dot(nn, normalize(L + Vv)), 0.0), 180.0);\n"
"            c += vec3(g * 1.6);\n"
"            gloss = 160.0; spec_k = 0.0;\n"
"        }\n" // ferrule: green when the line is live
"        else {\n"
"            // Ash grain runs the LENGTH of the cue as fine lines, so it has\n"
"            // to vary with the angle round the shaft and only drift slowly\n"
"            // along it. Varying it along the length instead — which is the\n"
"            // obvious reading of \"grain along the shaft\" — draws rings, and a\n"
"            // few hundred rings on a 13 mm shaft is a blur.\n"
"            float ang = a * 6.2831853;\n"
"            float lines = sin(ang * 23.0 + sin(t * 7.0) * 0.9);\n"
"            float fleck = sin(ang * 6.0 - t * 31.0);\n"
"            float g = clamp(0.5 + 0.30 * lines + 0.14 * fleck, 0.0, 1.0);\n"
"            c = mix(ash * 0.84, ash * 1.08, g);\n"
"            // Four-point hand splice. The ebony points rise OUT of the butt\n"
"            // and taper to a needle toward the tip — so they are widest at the\n"
"            // butt end, not the tip end, and at the base they very nearly meet\n"
"            // one another. Built the other way up and too narrow, they read as\n"
"            // four thin darts pointing the wrong way down the cue.\n"
"            float sp_tip = 0.560, sp_base = 0.815;\n"
"            if (t > sp_tip) {\n"
"                float k = clamp((t - sp_tip) / (sp_base - sp_tip), 0.0, 1.0);\n"
"                float halfw = 0.44 * pow(k, 0.80);\n"
"                float f = fract(a * 4.0);\n"
"                float d = min(f, 1.0 - f);\n"
"                c = mix(c, ebony, smoothstep(halfw, halfw * 0.86, d));\n"
"                // The accent veneer: a thin coloured line flashed down BOTH\n"
"                // edges of every point. On a real cue this is a sliver of dyed\n"
"                // wood between the splice and the shaft, and it is the detail\n"
"                // that makes the cue look made rather than turned.\n"
"                float edge = abs(d - halfw);\n"
"                float flash = 1.0 - smoothstep(0.0, halfw * 0.09 + 0.0035, edge);\n"
"                c = mix(c, u_caccent, flash * u_cflash * 0.95);\n"
"            }\n"
"            if (t > sp_base) {\n"
"                c = u_cbutt;\n"
"                // The inlaid BURR PANEL. A hand-spliced butt has a figured veneer\n"
"                // panel let into one side, bordered by the same veneer lines that\n"
"                // flash the splice points. Centred opposite the flat.\n"
"                float aa2 = a;\n"
"                float da = abs(fract(aa2 + 0.5) - 0.5);   // 0 at the far side\n"
"                if (u_cflash > 0.5 && t < 0.955) {\n"
"                    float pw = 0.105;\n"
"                    float inb = 1.0 - smoothstep(pw, pw * 0.92, da);\n"
"                    // figure across the panel\n"
"                    float f1 = texture(u_nap, vec2(t * 26.0, a * 3.0)).r;\n"
"                    float f2 = texture(u_nap, vec2(t * 90.0, a * 8.0)).g;\n"
"                    // FIGURED WOOD, not the veneer colour. The accent is the thin\n"
"                    // border line only; using it for the panel body turned the\n"
"                    // whole butt turquoise.\n"
"                    vec3 burr = u_cburr * (0.62 + 0.62 * f1) * (0.84 + 0.30 * f2);\n"
"                    c = mix(c, burr, inb);\n"
"                    // the veneer border lines down each edge of the panel\n"
"                    float ln = 1.0 - smoothstep(0.0, 0.012, abs(da - pw));\n"
"                    c = mix(c, u_caccent * 1.5, ln * 0.9);\n"
"                }\n"
"                // The BADGE PLATE, let into the flat: ivory, with four screws.\n"
"                float af = abs(fract(a + 0.5) - 0.5);     // 0 on the flat side? no:\n"
"                float ac = abs(a > 0.5 ? a - 1.0 : a);    // 0 at a == 0, the flat\n"
"                if (t > 0.905 && t < 0.958 && ac < 0.105) {\n"
"                    vec3 ivory = vec3(0.92, 0.90, 0.82);\n"
"                    float ex = smoothstep(0.105, 0.098, ac);\n"
"                    float ey = smoothstep(0.905, 0.912, t) * (1.0 - smoothstep(0.951, 0.958, t));\n"
"                    float pl = ex * ey;\n"
"                    c = mix(c, ivory, pl);\n"
"                    // four brass screws, one at each corner of the plate\n"
"                    vec2 sp2 = vec2((t - 0.9315) / 0.020, ac / 0.078);\n"
"                    vec2 g2 = abs(sp2) - vec2(0.72, 0.72);\n"
"                    float sd = length(max(g2, 0.0)) + min(max(g2.x, g2.y), 0.0);\n"
"                    float scr = 1.0 - smoothstep(0.10, 0.16, abs(sd));\n"
"                    c = mix(c, vec3(0.72, 0.58, 0.26), scr * pl * 0.95);\n"
"                    gloss = mix(gloss, 70.0, pl);\n"
"                }\n"
"            }\n"                        // solid butt above the splice
"            // Brass. A three-quarter jointed cue has a bright collar at the\n"
"            // joint and another at the butt cap, and both catch the light hard\n"
"            // enough to be a feature rather than a detail.\n"
"            float brass_lit = 0.0;\n"
"            // The butt collar sits just SHORT of the cap. Sitting on it, the\n"
"            // brass wrapped the rounded end and the cue finished in a gold\n"
"            // cone — a real one finishes in a dark rubber or horn cap with the\n"
"            // brass ring behind it.\n"
"            if ((t > 0.7240 && t < 0.7355) ||\n"
"                (t > 0.9600 && t < 0.9710)) {\n"
"                c = vec3(0.78, 0.62, 0.26);\n"
"                brass_lit = 1.0;\n"
"            }\n"
"            gloss = mix(gloss, 120.0, brass_lit);\n"
"            spec_k = mix(spec_k, 0.85, brass_lit);\n"  // brass collar
"        }\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), gloss);\n"
"        o_col = emit(to_linear(c) * (0.34 + 0.70 * d) + vec3(spec) * spec_k, 1.0, 1.0);\n"
"    } else if (u_mode == 7) {\n"
"        // Timber. The frame carries its own base colour per piece and a grain\n"
"        // coordinate that runs ALONG whichever length of wood the vertex\n"
"        // belongs to, so the grain follows the apron round the table and runs\n"
"        // UP the legs, instead of everything being striped in world space.\n"
"        vec3 base = to_linear(v_col);\n"
"        float g = v_uv.x;\n"
"        // Two octaves: close-spaced lines, and a slow wander so the figure is\n"
"        // not a barcode. Modulated across the grain so lines drift rather than\n"
"        // running dead straight for a metre.\n"
"        float fine = sin(g * 210.0 + sin(v_uv.y * 26.0) * 1.3);\n"
"        float slow = sin(g * 31.0 - v_uv.y * 3.0);\n"
"        float fig  = 0.5 + 0.30 * fine + 0.20 * slow;\n"
"        vec3 c = base * (0.80 + 0.42 * fig);\n"
"        vec3 n = normalize(v_nrm);\n"
"        float d = max(dot(n, L), 0.0);\n"
"        // French polish: a broad sheen plus a tight highlight, so the apron\n"
"        // catches the lamps the way varnished wood does.\n"
"        float spec = pow(max(dot(n, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), 26.0);\n"
"        o_col = emit(c * (0.26 + 0.72 * d) + vec3(spec) * 0.16, 1.0, 1.0);\n"
"    } else if (u_mode == 4) {\n"
"        // The table, with the handheld's own shading: colours authored per\n"
"        // triangle, lit by the ABSOLUTE dot with the overhead key. Absolute\n"
"        // because the mesh is double-sided — the cloth fan and the pocket\n"
"        // voids are wound for shading, not for a front-face convention.\n"
"        float ndl = abs(dot(normalize(v_nrm), L));\n"
"        vec3 tc = v_col;\n"
"        float wood_spec = 0.0;\n"
"        // The cushions, the jaws and the pocket throats are COVERED IN THE\n"
"        // SAME CLOTH as the bed, and they had no texture at all — flat slabs\n"
"        // of green next to a bed that had a nap, which is why the pockets\n"
"        // looked plastic. Anything close to the cloth colour gets the nap;\n"
"        // the rails and the woodwork do not.\n"
"        // Compare HUE, not absolute colour. The cushion faces and pocket\n"
"        // throats are the same cloth in a darker shade, so an absolute\n"
"        // distance test failed on exactly the surfaces that looked most\n"
"        // plastic. Normalising first makes it shade-independent, and it works\n"
"        // for a red or a blue cloth as well as a green one.\n"
"        float iscloth = 1.0 - smoothstep(0.06, 0.26,\n"
"            distance(normalize(v_col + 1e-4), normalize(u_cloth + 1e-4)));\n"
"        vec3 sn = normalize(v_nrm);\n"
"        if (iscloth > 0.01) {\n"
"            // Same model as the bed: bend the normal, let the sheen make the tone.\n"
"            NapSample nsc = nap_sample(v_local, v_nrm);\n"
"            sn = mix(sn, cloth_normal(v_nrm, nsc), iscloth);\n"
"            vec3 Vv = normalize(v_eyepos - v_world);\n"
"            vec3 Hv = normalize(L + Vv);\n"
"            float nh = max(dot(sn, Hv), 0.0);\n"
"            float nl = max(dot(sn, L), 0.0);\n"
"            float nv2 = max(abs(dot(sn, Vv)), 1e-3);\n"
"            float lost = lean_lost(nsc);\n"
"            float sh = sheen_charlie(nh, mix(0.26, 0.62, lost))\n"
"                     * sheen_vis(nl, nv2) * nl;\n"
"            vec3 sc = mix(to_linear(v_col) * 1.9, vec3(1.0), 0.30);\n"
"            float mocc = nap_occlusion(nsc);\n"
"            tc = v_col * (0.30 + 0.70 * abs(dot(sn, L)))\n"
"               * mix(1.0, mocc, iscloth);\n"
"            o_col = emit(to_linear(tc) + sc * sh * 0.85 * iscloth, 1.0, 1.0);\n"
"            return;\n"
"        }\n"
"        \n"
"        if (iscloth < 0.99) {\n"
"            // Board coordinates: along the rail and across it, on the surface itself.\n"
"            vec2 wq = surf_uv(v_local, v_nrm);\n"
"            // One low-frequency fetch for the turbulence, one high for grain and pores.\n"
"            vec2 warp = (texture(u_nap, wq / 0.55).rg - 0.5) * 2.0;\n"
"            vec2 fine = (texture(u_nap, vec2(wq.x / 0.22, wq.y / 0.020)).rg - 0.5) * 2.0;\n"
"            float varn = 0.0;\n"
"            tc = mix(tc, timber(v_col, wq, warp, fine, v_nrm, L, varn),\n"
"                     1.0 - iscloth);\n"
"            wood_spec = varn * (1.0 - iscloth);\n"
"        }\n"
"        \n"
"        // Specular ADDED after the diffuse, not folded into it.\n"
"        o_col = emit(to_linear(tc * (0.32 + 0.68 * ndl)) + vec3(wood_spec),\n"
"                     1.0, 1.0);\n"
"    } else if (u_mode == 8) {\n"
"        // The cloth, and the chalk on it. Rewritten whole rather than patched:\n"
"        // this block had accumulated three generations of nap experiment on top\n"
"        // of each other, including a duplicate declaration and a dead variable.\n"
"        vec2 q = v_local.xz;\n"
"        float aa = max(fwidth(v_local.x), fwidth(v_local.z)) * 1.2 + 1e-6;\n"
"        vec3 nv = normalize(v_nrm);\n"
"        vec3 V = normalize(v_eyepos - v_world);\n"
"\n"
"        // The nap: three octaves of the MIPMAPPED tile. Mipmapped is the whole\n"
"        // point — the hardware band-limits it per pixel AND per axis, so it can\n"
"        // be this fine without aliasing and it flattens on its own as you back\n"
"        // away. Everything I generated procedurally in here either aliased into\n"
"        // a swirling moire or, keyed to the pixel footprint, produced\n"
"        // axis-aligned square cells that read as blockiness under magnification.\n"
"        // The pile leans, and everything follows from that. No brightness noise:\n"
"        // the tone is the sheen lobe reading the bent normal, which is how velvet\n"
"        // gets soft sweeping variation rather than speckle.\n"
"        NapSample nsm = nap_sample(v_local, nv);\n"
"        vec3 N = cloth_normal(nv, nsm);\n"
"        vec3 cloth = to_linear(u_cloth);\n"
"        \n"
"        // Charlie sheen (Estevez and Kulla 2017 / glTF KHR_materials_sheen).\n"
"        vec3 Hv = normalize(L + V);\n"
"        float NdotH = max(dot(N, Hv), 0.0);\n"
"        float NdotL = max(dot(N, L), 0.0);\n"
"        float NdotV = max(abs(dot(N, V)), 1e-3);\n"
"        // Toksvig: the lobe widens by however much fine lean the mip chain ate,\n"
"        // so the cloth stays fuzzy at a distance instead of going flat.\n"
"        float lost = lean_lost(nsm);\n"
"        float shR = mix(0.26, 0.62, lost);\n"
"        float sh = sheen_charlie(NdotH, shR) * sheen_vis(NdotL, NdotV) * NdotL;\n"
"        vec3 sheenC = mix(cloth * 1.9, vec3(1.0), 0.30);\n"
"        float sheenAmt = sh * 0.85;\n"
"        float scal = 1.0 - max(max(sheenC.r, sheenC.g), sheenC.b) * 0.16;\n"
"        \n"
"        float m = mark_cov(q, aa);\n"
"        // Diffuse off the BENT normal too, so the lean shows in the body of\n"
"        // the colour and not only in the sheen.\n"
"        float ndl = abs(dot(N, L));\n"
"        float mocc = nap_occlusion(nsm);\n"
"        vec3 c = mix(cloth * mocc, to_linear(u_markc), m * 0.88);\n"
"        // Chalk is powder ON the fibres: it kills the sheen where it lands.\n"
"        // And a touch less ambient lift, so the cloth keeps its saturation in\n"
"        // the shadowed half instead of going grey.\n"
"        o_col = emit(c * (0.22 + 0.76 * ndl) * scal\n"
"                   + sheenC * sheenAmt * (1.0 - m * 0.85), 1.0, 1.0);\n"
"    } else if (u_mode == 9) {\n"
"        // One shell of the pile. The vertex shader has already lifted this copy\n"
"        // of the cloth by u_shell metres along the normal; u_furslice says which\n"
"        // slice of the volume belongs at that height. Strands that do not reach\n"
"        // this high simply are not in the slice, so the pile tapers.\n"
"        vec2 fq = v_local.xz / u_feltspan;\n"
"        vec2 fur = texture(u_fur, vec3(fq, u_furslice)).rg;\n"
"        float cov = fur.r;\n"
"        if (cov < 0.02) discard;\n"
"        float hh = (u_furslice + 0.5) / u_furslices;\n"
"        vec3 nv = normalize(v_nrm);\n"
"        vec3 V = normalize(v_eyepos - v_world);\n"
"        // Kajiya-Kay along the strand: a cylinder scatters in a cone about its\n"
"        // own axis, so the highlight runs ALONG a hair rather than dotting a\n"
"        // facet. With the hairs as geometry this is now shading a real strand.\n"
"        vec3 T3 = normalize(vec3(1.0, 0.55, 0.0));\n"
"        float TdL = dot(T3, L), TdV = dot(T3, V);\n"
"        float sL = sqrt(max(1.0 - TdL * TdL, 0.0));\n"
"        float sV = sqrt(max(1.0 - TdV * TdV, 0.0));\n"
"        float kk = pow(max(sL * sV - TdL * TdV, 0.0), 24.0);\n"
"        // Tips are lighter than roots: dye sits deeper at the base, and the\n"
"        // deeper hair is shadowed by everything above it.\n"
"        float lit = 0.80 + 0.24 * hh;\n"
"        float ndl = abs(dot(nv, L));\n"
"        // Close to the cloth's own colour on purpose. A shell should be\n"
"        // almost invisible looking straight down at it — the pile only\n"
"        // announces itself where you see THROUGH many shells at once,\n"
"        // which is the fuzzy silhouette over a pocket or a cushion. A\n"
"        // shell you can pick out on the flat is a stripe, not fur.\n"
"        vec3 c = to_linear(u_cloth) * (0.86 + 0.24 * fur.g) * lit\n"
"               * (0.55 + 0.45 * ndl) + vec3(kk * 0.05 * hh);\n"
"        // Chalk on the fibres. Slightly stronger up the strand, because\n"
"        // powder settles on the tips.\n"
"        float aa9 = max(fwidth(v_local.x), fwidth(v_local.z)) * 1.2 + 1e-6;\n"
"        float m9 = mark_cov(v_local.xz, aa9);\n"
"        c = mix(c, to_linear(u_markc) * (0.80 + 0.30 * hh), m9 * 0.92);\n"
"        if (u_furdbg > 0.5) { o_col = vec4(1.0, 0.0, 1.0, cov); return; }\n"
"        o_col = emit(c, cov, 0.0);\n"
"    } else if (u_mode == 10) {\n"
"        // A fin: one card of hair standing up out of the cloth. uv.y walks UP\n"
"        // the card, and walking up the card means walking up through the fur\n"
"        // volume — so the card carries the same strand profile the shells do,\n"
"        // thinning towards the tips, and the two agree with each other.\n"
"        float hv = v_uv.y;\n"
"        float sl = hv * (u_furslices - 1.0);\n"
"        vec2 fq = v_local.xz / u_feltspan;\n"
"        float cov = texture(u_fur, vec3(fq, floor(sl))).r;\n"
"        float cov2 = texture(u_fur, vec3(fq, min(floor(sl) + 1.0, u_furslices - 1.0))).r;\n"
"        cov = mix(cov, cov2, fract(sl));\n"
"        // Taper the card itself so a strand is a cone, and fade the very tips\n"
"        // so they do not end in a hard cut.\n"
"        float across = abs(v_uv.x - 0.5) * 2.0;\n"
"        cov *= (1.0 - across * across) * (1.0 - hv * 0.55);\n"
"        if (cov < 0.03) discard;\n"
"        vec3 nv = normalize(v_nrm);\n"
"        vec3 V = normalize(v_eyepos - v_world);\n"
"        vec3 T3 = normalize(vec3(1.0, 0.75, 0.0));\n"
"        float TdL = dot(T3, L), TdV = dot(T3, V);\n"
"        float sL = sqrt(max(1.0 - TdL * TdL, 0.0));\n"
"        float sV = sqrt(max(1.0 - TdV * TdV, 0.0));\n"
"        float kk = pow(max(sL * sV - TdL * TdV, 0.0), 22.0);\n"
"        float lit = 0.38 + 0.62 * hv;          // roots in shadow, tips in light\n"
"        vec3 c = to_linear(u_cloth) * lit * 1.05 + vec3(kk * (0.04 + 0.55 * hv));\n"
"        if (u_furdbg > 0.5) { o_col = vec4(1.0, 0.4, 0.0, cov); return; }\n"
"        o_col = emit(c, cov, 0.0);\n"
"    } else if (u_mode == 6) {\n"


"        // A ball's shadow on the cloth: a soft decal, as scene_add_shadow\n"
"        // draws it. Without these the balls hover.\n"
"        float d = length(v_uv - vec2(0.5)) * 2.0;\n"
"        float a = 0.55 * (1.0 - smoothstep(0.10, 1.0, d));\n"
"        o_col = emit(to_linear(u_clothsh) * 0.55, a, 0.0);\n"
"    } else {\n"
"        vec3 c = to_linear(u_colour.rgb);\n"
"        float d = max(dot(v_nrm, L), 0.0);\n"
"        o_col = emit(c * (0.42 + 0.62 * d), u_colour.a, 1.0);\n"
"    }\n"
"}\n";

/* ---- mesh building ------------------------------------------------------ */

typedef struct { float p[3], n[3], uv[2], c[3]; } Vtx;

typedef struct {
    Vtx     *v;
    uint16_t *i;
    int nv, ni, cap_v, cap_i;
} Builder;

static void b_init(Builder *b, int cv, int ci) {
    b->v = malloc(sizeof(Vtx) * (size_t)cv);
    b->i = malloc(sizeof(uint16_t) * (size_t)ci);
    b->nv = b->ni = 0; b->cap_v = cv; b->cap_i = ci;
}
static void b_free(Builder *b) { free(b->v); free(b->i); b->v = NULL; b->i = NULL; }

static int b_vert(Builder *b, float x, float y, float z,
                  float nx, float ny, float nz, float u, float v) {
    if (b->nv >= b->cap_v) return b->nv ? b->nv - 1 : 0;
    Vtx *t = &b->v[b->nv];
    t->p[0]=x; t->p[1]=y; t->p[2]=z;
    t->n[0]=nx; t->n[1]=ny; t->n[2]=nz;
    t->uv[0]=u; t->uv[1]=v;
    t->c[0]=t->c[1]=t->c[2]=1.0f;
    return b->nv++;
}
static void b_tri(Builder *b, int a, int c, int d) {
    if (b->ni + 3 > b->cap_i) return;
    b->i[b->ni++] = (uint16_t)a; b->i[b->ni++] = (uint16_t)c; b->i[b->ni++] = (uint16_t)d;
}
static void b_quad(Builder *b, int a, int c, int d, int e) { b_tri(b, a, c, d); b_tri(b, a, d, e); }

/* A flat quad with an explicit normal.
 *
 * The winding is derived rather than trusted. Every face here is described by
 * four corners and the direction it faces, and getting the corner order to
 * agree with that direction by hand — for a cloth in the XZ plane, a cushion
 * face leaning at a pocket angle, a rail skirt — is a mistake waiting at every
 * call site, and it fails silently: the face simply is not there, and you go
 * looking for it in the shader. So compare the corners' own winding against the
 * normal and swap if they disagree. Then a face is always visible from the side
 * it says it faces. */
static void b_face(Builder *b, const float *p0, const float *p1,
                   const float *p2, const float *p3, const float *n) {
    float u[3] = { p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2] };
    float v[3] = { p2[0]-p0[0], p2[1]-p0[1], p2[2]-p0[2] };
    float c[3] = { u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0] };
    int flip = (c[0]*n[0] + c[1]*n[1] + c[2]*n[2]) < 0.0f;
    const float *q[4] = { p0, p1, p2, p3 };
    int idx[4];
    for (int k = 0; k < 4; k++) {
        const float *p = q[flip ? 3 - k : k];
        idx[k] = b_vert(b, p[0], p[1], p[2], n[0], n[1], n[2],
                        (k == 1 || k == 2) ? 1.0f : 0.0f,
                        (k >= 2) ? 1.0f : 0.0f);
    }
    b_quad(b, idx[0], idx[1], idx[2], idx[3]);
}

typedef struct { GLuint vao, vbo, ibo; int n; } Mesh;

static void mesh_upload(Mesh *m, const Builder *b) {
    glGenVertexArrays(1, &m->vao);
    glBindVertexArray(m->vao);
    glGenBuffers(1, &m->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->nv * (GLsizeiptr)sizeof(Vtx), b->v, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)12);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)24);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)32);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glGenBuffers(1, &m->ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m->ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)b->ni * 2, b->i, GL_STATIC_DRAW);
    m->n = b->ni;
    glBindVertexArray(0);
}

static void mesh_free(Mesh *m) {
    if (!m->vao) return;
    glDeleteBuffers(1, &m->vbo);
    glDeleteBuffers(1, &m->ibo);
    glDeleteVertexArrays(1, &m->vao);
    memset(m, 0, sizeof *m);
}

/* ---- state -------------------------------------------------------------- */

static struct {
    GLuint prog;
    GLint  u_mvp, u_model, u_tex, u_mode, u_encode, u_colour, u_colour2, u_light;
    GLint  u_ballslice, u_balls, u_clothsh;
    GLint  u_cloth, u_fur, u_nap, u_feltspan, u_furslice, u_furslices, u_furdbg, u_shell,
           u_cshaft, u_csplice, u_caccent, u_cbutt, u_cburr, u_cflash, u_markc, u_baulk, u_drad, u_linew, u_spotr, u_nspot, u_spots;
    GLint  u_lampC, u_lampX, u_lampZ, u_nlamp, u_eye;
    Mesh   ctrl[2];
    Mesh   fins, bed, table, lips, frame, ball, cue, quad, floor, grip;
    int    frame_sel;
    GLuint ball_tex;      /* equirect atlas, one slice per ball id */
    float  fur_scale;
    GLuint nap_tex;
    GLuint fur_tex;       /* fur volume: FUR_SLICES layers of strand coverage */
    GLuint hud_tex;
    int    encode;
    int    ready;
    CueTable tab;
    void *tab_buf, *stri_buf;
} G;

/* The multiview header, prepended at compile time.
 *
 * The same shader source has to work both ways: multiview needs the extension, the
 * num_views layout and gl_ViewID_OVR, and the per-eye fallback has none of those.
 * A VIEW_ID macro is the whole difference — the body indexes u_mvp[VIEW_ID] and
 * u_eye[VIEW_ID] either way, and VIEW_ID is 0 when there is only one view per pass.
 *
 * gl_ViewID_OVR is a VERTEX stage builtin, so the eye position is resolved there
 * and handed to the fragment stage as a varying rather than read from a uniform. */
static int s_mv_shader;      /* compile the multiview variant */

static GLuint compile(GLenum type, const char *src) {
    const char *hdr = s_mv_shader
        ? "#version 300 es\n"
          "#extension GL_OVR_multiview2 : require\n"
          "layout(num_views = 2) in;\n"
          "#define VIEW_ID int(gl_ViewID_OVR)\n"
        : "#version 300 es\n"
          "#define VIEW_ID 0\n";
    /* the layout(num_views) qualifier is vertex-only */
    const char *hdr_fs = s_mv_shader
        ? "#version 300 es\n"
          "#extension GL_OVR_multiview2 : require\n"
          "#define VIEW_ID int(gl_ViewID_OVR)\n"
        : "#version 300 es\n"
          "#define VIEW_ID 0\n";
    const char *parts[2] = { (type == GL_VERTEX_SHADER) ? hdr : hdr_fs, src };
    GLuint s = glCreateShader(type);
    glShaderSource(s, 2, parts, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char l[1024]; glGetShaderInfoLog(s, sizeof l, NULL, l); LOGI("[cuevr] shader: %s", l); return 0; }
    return s;
}

/* ---- the table ---------------------------------------------------------- */

/* A UV sphere of unit radius. Enough segments that a ball 26 mm across still
 * reads as round with your eye 40 cm from it. */
static void build_sphere(Builder *b, int slices, int stacks) {
    for (int j = 0; j <= stacks; j++) {
        float v = (float)j / stacks, phi = v * PI;
        for (int i = 0; i <= slices; i++) {
            float u = (float)i / slices, th = u * 2.0f * PI;
            float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
            b_vert(b, x, y, z, x, y, z, u, v);
        }
    }
    /* Wind outward. The obvious ordering here (a, c, c+1, a+1) walks DOWN a
     * stack before going round, which puts the face normal on the INSIDE of the
     * sphere: with culling on, every near surface is discarded, so you see the
     * cloth and its markings straight through the ball and the lamp highlight
     * lands on the far inner wall — which reads as reflections stuck under the
     * ball rather than as a winding bug. Go round first, then down. */
    for (int j = 0; j < stacks; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, a + 1, c + 1, c);
        }
}

/* A cue, turned on a lathe.
 *
 * The one thing here that is NOT the handheld's — its cue is drawn in screen
 * space as part of the aim overlay, so there is no mesh to borrow. A cue is
 * also the object closest to your face for the whole game, so it earns the
 * geometry.
 *
 * Built from a real cue's profile along its 1.45 m: a 10 mm leather tip, a
 * brass-white ferrule, an ash shaft swelling from 9.5 mm to 19 mm, then the
 * ebony butt out to 29 mm with a rounded cap. The four-point hand splice and
 * the grain are shaded in cuevr's fragment shader from the axial and angular
 * coordinates carried in uv, so the cue needs no texture at all.
 *
 * y = 0 is the TIP and +Y runs back to the butt, which is how it is positioned:
 * the tip goes where the cue line meets the ball and the rest follows behind,
 * through and past your hands, as a real cue does. */
#define CUE_LEN 1.45f

/* radius (m) at a fraction along the cue, tip to butt */
static float cue_radius(float t) {
    const float x = t * CUE_LEN;
    /* The tip is a shallow spherical cap, and the numbers matter. A snooker tip
     * is about 10 mm across, domed to roughly the curve of a sixpence — a
     * curvature radius of ~10.5 mm. On a 5.1 mm tip radius that cap is only
     * 1.32 mm DEEP:
     *
     *     h = Rc - sqrt(Rc^2 - Rt^2) = 10.5 - sqrt(110.25 - 26.01) = 1.32 mm
     *
     * and the profile through it is r = sqrt(2*Rc*x - x^2).
     *
     * The version before this made up its own curve and tapered from 5.1 mm down
     * to 1.4 mm over a full 3 mm, which is not a domed tip, it is a sharpened
     * pencil. Worth doing the arithmetic rather than picking a shape. */
    const float TIP_R = 0.0051f;      /* tip radius: a 10.2 mm tip */
    const float TIP_RC = 0.0105f;     /* curvature radius of the dome */
    const float DOME = 0.00132f;      /* therefore this deep */
    if (x < DOME) {
        float r = sqrtf(2.0f * TIP_RC * x - x * x);
        return r > TIP_R ? TIP_R : r;
    }
    if (x < 0.0035f) return TIP_R;                                   /* leather pad */
    if (x < 0.0135f) return 0.00515f;                                /* 10 mm ferrule */
    if (x < 1.100f) {                                                /* ash shaft */
        float k = (x - 0.032f) / (1.100f - 0.032f);
        return 0.0051f + k * (0.0095f - 0.0051f);
    }
    if (x < 1.410f) {                                                /* ebony butt */
        float k = (x - 1.100f) / (1.410f - 1.100f);
        return 0.0095f + k * (0.0145f - 0.0095f);
    }
    /* rounded butt cap */
    float k = (x - 1.410f) / (CUE_LEN - 1.410f);
    if (k > 1.0f) k = 1.0f;
    return 0.0145f * sqrtf(1.0f - k * k * 0.95f);
}

/* A real cue is NOT a solid of revolution. The butt of a hand-spliced cue carries
 * a FLAT down one side — it is planed off so the badge plate can be let into it, and
 * it is also what stops the cue rolling off the table and tells your hand which way
 * up it is. Lathing a perfect cylinder is the giveaway that a cue was turned by a
 * computer rather than made.
 *
 * The flat starts where the splice ends and runs to the butt cap, deepening as the
 * cue thickens so its width stays roughly constant. */
static float cue_flat_scale(float t, float ang) {
    const float F0 = 0.815f, F1 = 0.975f;      /* splice base -> just short of the cap */
    if (t < F0 || t > F1) return 1.0f;
    /* Centred on ang = 0, about 78 degrees wide. */
    float a = ang;
    while (a >  PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    float half = 0.68f;
    if (fabsf(a) > half) return 1.0f;
    /* Fade in and out along the length so it does not start with a step. */
    float lz = (t - F0) / 0.05f;  if (lz > 1.0f) lz = 1.0f;
    float lo = (F1 - t) / 0.05f;  if (lo > 1.0f) lo = 1.0f;
    float run = lz < lo ? lz : lo;
    /* A chord across the circle: the surface is planar, so the radius at angle a
     * is cos(half)/cos(a) of the full radius. */
    float chord = cosf(half) / cosf(a);
    return 1.0f - (1.0f - chord) * run;
}

static void build_cue(Builder *b, int slices, int rings) {
    for (int j = 0; j <= rings; j++) {
        /* Rings, and the distribution is the whole reason the tip looked like a
         * sharpened pencil. The old curve put ring 0 at x=0 and ring 1 at 13.9 mm,
         * so a 1.32 mm dome had NOTHING sampling it and the lathe ran a straight
         * cone from the apex to full shaft radius. The profile function was right
         * and unreachable.
         *
         * Two zones: the first quarter of the rings resolve the first 15 mm — the
         * dome, the leather and the ferrule, where the shape changes and where your
         * eye actually is — and the rest carry the shaft and butt, which are very
         * nearly straight and need almost nothing. */
        float t;
        {
            /* THREE zones, because both ends are rounded and the middle is very
             * nearly straight. The version before this had two, and the butt cap
             * got exactly one ring across its last 40 mm — so the cue finished in
             * a black cone, the identical fault the tip had. Whenever the profile
             * curves, the rings have to be there to see it. */
            int tipr  = rings / 4;  if (tipr  < 10) tipr  = 10;
            int buttr = rings / 6;  if (buttr < 6)  buttr = 6;
            int mid   = rings - tipr - buttr;
            if (mid < 4) mid = 4;
            const float TIPZ = 0.015f, BUTTZ = CUE_LEN - 0.050f;
            float x;
            if (j <= tipr) {
                float u = (float)j / (float)tipr;
                x = TIPZ * u * u;                       /* dense at the tip */
            } else if (j <= tipr + mid) {
                float v = (float)(j - tipr) / (float)mid;
                x = TIPZ + (BUTTZ - TIPZ) * v;          /* the straight run */
            } else {
                float v = (float)(j - tipr - mid) / (float)buttr;
                x = BUTTZ + (CUE_LEN - BUTTZ) * v;      /* the rounded cap */
            }
            t = x / CUE_LEN;
        }
        float y = t * CUE_LEN, r0 = cue_radius(t);
        float rn = cue_radius(t + 0.004f > 1.0f ? 1.0f : t + 0.004f);
        float slope0 = (rn - r0) / (0.004f * CUE_LEN);
        for (int i = 0; i <= slices; i++) {
            float u = (float)i / slices, th = u * 2.0f * PI;
            float cx = cosf(th), cz = sinf(th);
            /* The flat makes the radius depend on the ANGLE as well as the length. */
            float fs = cue_flat_scale(t, th);
            float r = r0 * fs;
            float slope = slope0 * fs;
            /* Across-section slope where the flat meets the round, so the edge of
             * the flat lights as an edge instead of vanishing. */
            float fs2 = cue_flat_scale(t, th + 0.06f);
            float dtan = (fs2 - fs) * r0 / (0.06f * r0 + 1e-6f);
            float nl = 1.0f / sqrtf(1.0f + slope * slope + dtan * dtan);
            b_vert(b, cx * r, y, cz * r,
                   (cx - (-cz) * dtan) * nl, -slope * nl, (cz - cx * dtan) * nl, u, t);
        }
    }
    /* Note the order is the REVERSE of build_sphere's, and has to be: the
     * sphere's rings run from +Y down (y = cos(phi)) while the cue's run from
     * the tip up (y = t * CUE_LEN), so the identical index pattern produces
     * opposite handedness. Getting it wrong here made the cue hollow — you saw
     * its inside wall, lit by a normal pointing the wrong way. */
    for (int j = 0; j < rings; j++)
        for (int i = 0; i < slices; i++) {
            int a = j * (slices + 1) + i, c = a + slices + 1;
            b_quad(b, a, c, c + 1, a + 1);
        }
}

/* The table, exactly as the handheld builds it.
 *
 * cue_render_build_table() emits a world-space triangle soup — cloth bed fanned
 * over the real knuckle boundary, K66 cushion cross-sections with the
 * overhanging nose and its undercut, splayed jaw facings, pocket voids, drop
 * lips, baulk line, D and spots — each triangle carrying its own authored
 * colour. Those shapes were tuned table by table and they are what the game is,
 * so nothing here reshapes them: the triangles are copied into a vertex buffer
 * and handed to GL as they come.
 *
 * The handheld's own draw order is preserved too: [0, lip) normally, then the
 * drop lips last with depth writes off, so a ball sitting in a pocket covers
 * them rather than being covered.
 */
/* ---- smoothed normals ----------------------------------------------------- *
 * cue_render gives every triangle a FLAT face normal — t->nrm = cross(edge, edge) —
 * and the builder used to hand that same normal to all three vertices. So the table
 * was flat-shaded throughout, with no interpolation anywhere. On a 128x128 screen
 * that is invisible. At thirty centimetres it is the faceting, and it was also
 * quietly wrecking the cloth: the nap's texture frame is derived from the normal, so
 * a normal that jumps at every triangle boundary makes the texture orientation jump
 * with it, which is why the cushion tops showed blocky patches the size of their
 * triangles while the flat bed beside them looked fine.
 *
 * Averaging normals across triangles that share a position fixes both. Creases have
 * to survive though — the join between a cushion's undercut face and its top is a
 * real edge, not a smoothing error — so a face only contributes to a vertex whose
 * own face points within CREASE degrees of it. That is the standard treatment and
 * it keeps the sharp edges sharp.
 *
 * A spatial hash on the quantised position, because the mesh is a few thousand
 * triangles and O(n^2) over their vertices would not be. */
#define SMOOTH_CREASE_COS 0.62f      /* ~52 degrees */
#define SMOOTH_BUCKETS    8192

typedef struct { int tri, corner, next; } SmoothRef;

static unsigned smooth_hash(const Vec3 *p) {
    /* 0.1 mm buckets: fine enough that distinct features never share one, coarse
     * enough that the same welded corner always does. */
    int x = (int)floorf(p->x * 10000.0f);
    int y = (int)floorf(p->y * 10000.0f);
    int z = (int)floorf(p->z * 10000.0f);
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ (unsigned)z * 83492791u;
    return h & (SMOOTH_BUCKETS - 1);
}

static void build_from_cue_render(Builder *b, const CueTri *tri_, int from, int to) {
    const int n = to - from;
    if (n <= 0) return;

    int *head = (int *)malloc(sizeof(int) * SMOOTH_BUCKETS);
    SmoothRef *refs = (SmoothRef *)malloc(sizeof(SmoothRef) * (size_t)n * 3);
    if (!head || !refs) {          /* no memory: flat normals, as before */
        free(head); free(refs);
        for (int i = from; i < to; i++) {
            const CueTri *t = &tri_[i];
            float r = ((t->color >> 11) & 31) / 31.0f;
            float g = ((t->color >> 5) & 63) / 63.0f;
            float bl = (t->color & 31) / 31.0f;
            int idx[3];
            for (int k = 0; k < 3; k++) {
                idx[k] = b_vert(b, t->v[k].x, t->v[k].y, t->v[k].z,
                                t->nrm.x, t->nrm.y, t->nrm.z, 0.0f, 0.0f);
                Vtx *vx = &b->v[idx[k]];
                vx->c[0] = r; vx->c[1] = g; vx->c[2] = bl;
            }
            b_tri(b, idx[0], idx[1], idx[2]);
        }
        return;
    }
    for (int i = 0; i < SMOOTH_BUCKETS; i++) head[i] = -1;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) {
            unsigned h = smooth_hash(&tri_[from + i].v[k]);
            int r = i * 3 + k;
            refs[r].tri = i; refs[r].corner = k;
            refs[r].next = head[h]; head[h] = r;
        }
    }

    for (int i = 0; i < n; i++) {
        const CueTri *t = &tri_[from + i];
        float r = ((t->color >> 11) & 31) / 31.0f;
        float g = ((t->color >> 5) & 63) / 63.0f;
        float bl = (t->color & 31) / 31.0f;
        int idx[3];
        for (int k = 0; k < 3; k++) {
            Vec3 acc = t->nrm;
            for (int e = head[smooth_hash(&t->v[k])]; e >= 0; e = refs[e].next) {
                if (refs[e].tri == i) continue;
                const CueTri *o = &tri_[from + refs[e].tri];
                Vec3 op = o->v[refs[e].corner];
                if (fabsf(op.x - t->v[k].x) > 1e-4f ||
                    fabsf(op.y - t->v[k].y) > 1e-4f ||
                    fabsf(op.z - t->v[k].z) > 1e-4f) continue;
                /* Only across a smooth join, so real creases stay crisp. */
                if (v3_dot(o->nrm, t->nrm) < SMOOTH_CREASE_COS) continue;
                acc = v3_add(acc, o->nrm);
            }
            Vec3 nn = v3_len(acc) > 1e-5f ? v3_norm(acc) : t->nrm;
            idx[k] = b_vert(b, t->v[k].x, t->v[k].y, t->v[k].z,
                            nn.x, nn.y, nn.z, 0.0f, 0.0f);
            Vtx *vx = &b->v[idx[k]];
            vx->c[0] = r; vx->c[1] = g; vx->c[2] = bl;
        }
        b_tri(b, idx[0], idx[1], idx[2]);
    }
    free(head); free(refs);
}

/* The balls, exactly as the handheld paints them.
 *
 * cue_render shades every ball through ball_sample(id, nb, base), a function of
 * the BALL-LOCAL normal — which is to say it is already a texture over the
 * sphere, complete with the numbers, the stripes, the spots and whichever of
 * the five authored ball sets is selected. So bake it: sample that function
 * over an equirectangular grid once per ball id at start-up and upload the lot
 * as one atlas, a slice per id. The fragment shader then looks up the same
 * colour the handheld would have computed, and because the lookup is by
 * ball-local direction it rotates with the ball's own orientation matrix —
 * the markings roll exactly as they should.
 *
 * An earlier version of this file invented its own palette and drew stripes
 * with a smoothstep. It looked plausible and it was not the game. */
/* 256x128 per ball, up from 64x32. ball_sample() is analytic — sharp thresholds
 * on the ball-local normal — so it has no native resolution of its own and the
 * old bake was simply discarding detail it could have had. At 128x128 on the
 * handheld a ball was a few dozen pixels across and 64x32 was ample; in VR you
 * can put your eye next to one. */
/* 512x256 per ball. The number patch covers only about a quarter of the sphere's
 * longitude and latitude, so at 256x128 the circle was a mere 37 texels across
 * and the digit inside it was maybe 20 — which is why it looked low-resolution
 * however good the font was. At 512x256 the circle is 74 texels and the digit
 * reads properly. 26 balls at RG565 plus mips is about 7 MB, which is affordable
 * for the thing you spend the whole game looking at. */
#define BTEX_W   512
#define BTEX_H   256
/* Derived, not chosen. Snooker ids run past the pool set — cue, 1..15 reds,
 * then CUE_ID_YELLOW(20) through CUE_ID_BLACK(25) — and picking a round number
 * here instead of reading the enum is how pink and black ended up sharing the
 * cue ball's slice. */
#define BTEX_IDS (CUE_ID_BLACK + 1)

/* Re-baked whenever the table changes, NOT once at start-up.
 *
 * ball_sample() answers differently depending on s_is_snooker and the selected
 * ball set, and cue_render_build_table() is what sets the former. Baking once
 * meant every snooker frame was played with pool balls: ids 1..15 came out as
 * solids and stripes rather than reds. The original never had this failure
 * mode because it evaluates the function per pixel; the cache is mine, so
 * keeping it in step is mine too. */
/* ---- the fur volume ------------------------------------------------------ *
 * Shells and fins, after Lengyel et al. 2001 and NVIDIA's WP-03021 write-up of
 * it. This is the standard interactive fur method and it is a VOLUME technique,
 * which is the thing my previous two attempts got wrong: a normal map is a
 * picture of fur, and no amount of filtering turns a picture into geometry that
 * occludes itself.
 *
 * The volume is a stack of 2D slices through a field of hair. Slice k contains
 * the cross-sections of every strand tall enough to reach height k, so the
 * strands thin out as you go up. At draw time the cloth is rendered n times,
 * each pass extruded along the normal by its share of the pile height and
 * textured with its own slice — so the hairs are real geometry at real heights,
 * and a hair nearer the eye really does hide one behind it.
 *
 * A texture array, one layer per slice, which is exactly what the DirectX 10
 * sample used it for: no rebinding between shell passes. */
#define FUR_N      256      /* slice resolution */
#define FUR_SLICES 8        /* shells through the pile */
/* 18 mm per tile: 256 texels across it is 0.07 mm a texel, so the grain is
 * FINE — dense velvet rather than countable hairs. The reference photograph is
 * the argument: a real cloth close up has no resolvable strands on the flat, it
 * has a uniform suede nap, and the only place the pile announces itself is the
 * soft fuzzy silhouette where the cloth turns away over a pocket. */
#define FUR_SPAN   0.011f   /* metres of cloth per tile */
#define FUR_PILE   0.0022f  /* total pile height in metres */

static float fur_hash(int x, int y, int px, int py) {
    x = ((x % px) + px) % px;
    y = ((y % py) + py) % py;
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}
static float fur_noise(float x, float y, int px, int py) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = fur_hash(xi, yi, px, py), b = fur_hash(xi + 1, yi, px, py);
    float c = fur_hash(xi, yi + 1, px, py), d = fur_hash(xi + 1, yi + 1, px, py);
    float ab = a + (b - a) * fx, cd = c + (d - c) * fx;
    return ab + (cd - ab) * fy;
}

/* ---- the cue rack -------------------------------------------------------- *
 * Loosely after the British ranges — an ash-and-ebony hand splice is the default
 * and everything else is a variation on it. The accent veneer is the thin
 * coloured line flashed down each side of a splice point, which is the detail
 * that makes a cue look made rather than turned. */
static const CueVrCueDesign CUE_RACK[] = {
  /*  name            shaft                splice                  accent (veneer)        butt                    burr panel            flash */
  { "ASH & EBONY",  {0.86f,0.74f,0.54f}, {0.085f,0.065f,0.055f}, {0.085f,0.065f,0.055f}, {0.085f,0.065f,0.055f}, {0.085f,0.065f,0.055f}, 0 },
  { "TURQUOISE",    {0.84f,0.72f,0.52f}, {0.10f,0.075f,0.06f},   {0.20f,0.72f,0.74f},    {0.055f,0.05f,0.05f},   {0.42f,0.26f,0.13f},    1 },
  { "BURR WALNUT",  {0.87f,0.75f,0.55f}, {0.34f,0.20f,0.10f},    {0.72f,0.56f,0.30f},    {0.26f,0.15f,0.08f},    {0.52f,0.34f,0.16f},    1 },
  { "EBONY PRO",    {0.72f,0.61f,0.45f}, {0.06f,0.05f,0.05f},    {0.80f,0.80f,0.82f},    {0.045f,0.04f,0.04f},   {0.30f,0.22f,0.16f},    1 },
  { "MAPLE & BLUE", {0.93f,0.86f,0.70f}, {0.09f,0.07f,0.07f},    {0.18f,0.38f,0.80f},    {0.07f,0.07f,0.09f},    {0.38f,0.30f,0.22f},    1 },
  { "ROSEWOOD",     {0.82f,0.68f,0.48f}, {0.36f,0.13f,0.11f},    {0.78f,0.34f,0.20f},    {0.24f,0.09f,0.08f},    {0.46f,0.20f,0.13f},    1 },
  { "TOURNAMENT",   {0.90f,0.82f,0.63f}, {0.08f,0.06f,0.055f},   {0.16f,0.52f,0.28f},    {0.06f,0.055f,0.05f},   {0.34f,0.24f,0.14f},    1 },
};
#define CUE_RACK_N ((int)(sizeof CUE_RACK / sizeof CUE_RACK[0]))
static int s_cue_sel;

int         cuevr_render_cue_count(void) { return CUE_RACK_N; }
const char *cuevr_render_cue_name(int i) {
    return (i >= 0 && i < CUE_RACK_N) ? CUE_RACK[i].name : "";
}
void cuevr_render_set_cue(int i) { s_cue_sel = (i < 0 || i >= CUE_RACK_N) ? 0 : i; }
int         cuevr_render_cue(void) { return s_cue_sel; }

/* ---- the nap: a balanced pile-LEAN field --------------------------------- *
 * Not a coverage mask. The fur volume's slice 0 is mostly 1.0 with dark gaps
 * between strands, so subtracting its mean gave small positives and occasional
 * large negatives — black speckle over flat green, which is exactly what it
 * looked like. A coverage mask is the wrong source for a nap.
 *
 * And brightness is the wrong thing to modulate. On real velvet the tone comes
 * from the pile LEANING: the sheen lobe peaks at grazing angles, so a small change
 * in fibre direction swings the brightness a long way, which is what produces
 * those soft sweeping tonal bands instead of dots. So this bakes a two-component
 * signed vector field — the direction the pile leans — and the shader perturbs the
 * shading normal with it and lets the BRDF do the rest.
 *
 * RG8, zero-mean, several octaves, isotropic, tiling, mipmapped. */
/* 1024, and seven octaves. The point of a bigger tile is not resolution for its own
 * sake — it is that ONE fetch then contains detail from 6 mm down to 0.09 mm, and
 * the pattern does not repeat until 45 mm. The small tile gave a single fetch few
 * octaves (bland) repeating every 11 mm (repetitive), and I had been buying the
 * detail back with extra fetches, which is what cost the frame rate. Bake it in
 * once instead. 2 MB, mipmapped. */
#define NAP_N 1024
#define NAP_SPAN 0.045f

static void bake_nap(void) {
    if (G.nap_tex) { glDeleteTextures(1, &G.nap_tex); G.nap_tex = 0; }
    uint8_t *px = (uint8_t *)malloc((size_t)NAP_N * NAP_N * 2);
    if (!px) { LOGI("[cuevr] no memory for the nap field"); return; }
    for (int y = 0; y < NAP_N; y++) {
        for (int x = 0; x < NAP_N; x++) {
            float u = (float)x / NAP_N, v = (float)y / NAP_N;
            float ax = 0.0f, az = 0.0f, amp = 1.0f, norm = 0.0f;
            int per = 8;
            /* Seven octaves, and the WEIGHTING is the thing that matters. Standard
             * 1/f halves the amplitude each octave, so the coarsest dominates and
             * the field reads as cloud — which is exactly how the bed looked. Cloth
             * is the other way round: almost all of its energy is at the fine end,
             * with only a whisper of large-scale unevenness. A gentle 0.86 falloff
             * over seven octaves puts the weight where the weave is. */
            for (int o = 0; o < 7; o++) {
                ax += (fur_noise(u * per, v * per, per, per) - 0.5f) * amp;
                az += (fur_noise(u * per + 37.0f, v * per + 11.0f, per, per) - 0.5f) * amp;
                norm += amp;
                amp *= 0.86f;
                per *= 2;
            }
            ax /= norm; az /= norm;                 /* back to about -0.5..0.5 */
            int r = (int)((ax + 0.5f) * 255.0f + 0.5f);
            int g = (int)((az + 0.5f) * 255.0f + 0.5f);
            px[((size_t)y * NAP_N + x) * 2 + 0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            px[((size_t)y * NAP_N + x) * 2 + 1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
        }
    }
    glGenTextures(1, &G.nap_tex);
    glBindTexture(GL_TEXTURE_2D, G.nap_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, NAP_N, NAP_N, 0, GL_RG, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    {   GLfloat aniso = 0.0f;
        glGetFloatv(0x84FF, &aniso);
        if (aniso > 1.0f) { if (aniso > 16.0f) aniso = 16.0f;
            glTexParameterf(GL_TEXTURE_2D, 0x84FE, aniso); }
        while (glGetError() != GL_NO_ERROR) { }
    }
    free(px);
    LOGI("[cuevr] baked a %dx%d nap lean field (7 octaves, %.0f mm tile)",
         NAP_N, NAP_N, (double)(NAP_SPAN * 1000.0f));
}

static void bake_fur(void) {
    if (G.fur_tex) { glDeleteTextures(1, &G.fur_tex); G.fur_tex = 0; }
    /* RG: coverage, and a per-strand shade so they are not all identical. */
    uint8_t *px = (uint8_t *)malloc((size_t)FUR_N * FUR_N * FUR_SLICES * 2);
    if (!px) { LOGI("[cuevr] no memory for the fur volume"); return; }

    /* Near-isotropic, because the reference photograph is: measured over a flat
     * lit patch its horizontal and vertical pixel gradients differ by 16%, not by
     * the 10:1 a strongly combed nap would give. Real baize at this scale reads as
     * an even suede, and the strong directionality I had made the grain 1.4x
     * harsher across the lay than along it — the wrong way round as well. */
    /* Sized in millimetres of cloth: the reference macro photo is ~0.2 mm/pixel
     * with grain of one or two pixels, so the weave is about 0.3 mm. An 11 mm tile
     * over ~36 periods lands there. Matching it in SCREEN pixels instead is how I
     * ended up at 0.08 mm, twelve times below a pixel and averaged to a flat fill. */
    const int PA = 34, PC = 38;         /* periods along / across the tile */
    for (int y = 0; y < FUR_N; y++) {
        for (int x = 0; x < FUR_N; x++) {
            float u = (float)x / FUR_N, v = (float)y / FUR_N;
            /* The lay wanders, baked in rather than rotated per pixel. */
            float warp = (fur_noise(u * 4.0f, v * 4.0f, 4, 4) - 0.5f) * 0.10f;
            float vw = v + warp;
            /* Strand density: fine across the nap, drawn out along it. */
            float d = fur_noise(u * PA, vw * PC, PA, PC) * 0.7f
                    + fur_noise(u * PA * 3 + 5.0f, vw * PC * 2 + 3.0f, PA * 3, PC * 2) * 0.3f;
            /* How TALL this strand is. Not every hair reaches the top — that
             * variation is what stops the pile looking like a solid slab. */
            float tall = fur_noise(u * PA * 2 + 17.0f, vw * PC + 29.0f, PA * 2, PC) ;
            float shade = fur_noise(u * PC + 41.0f, vw * PA + 13.0f, PC, PA);
            for (int k = 0; k < FUR_SLICES; k++) {
                float hh = ((float)k + 0.5f) / FUR_SLICES;   /* height of this slice */
                /* A strand exists in this slice if it is dense enough AND tall
                 * enough to reach it. Tapering the threshold with height is what
                 * makes a hair a cone rather than a post. */
                /* Thresholds set from the MEASURED distribution of dd, not from
                 * guesses: the first attempt asked for more density than the noise
                 * ever produced, so every slice clamped to zero and the shells
                 * drew nothing at all. Baize is nearly solid at the roots and
                 * thins to a few tips, so slice 0 wants ~90% coverage and the top
                 * slice a few percent. */
                float need = 0.13f + 0.50f * hh;
                float dd = d * (0.55f + 0.45f * tall);
                float cov = (dd - need) / 0.10f;
                if (cov < 0.0f) cov = 0.0f;
                if (cov > 1.0f) cov = 1.0f;
                if (tall < hh * 0.55f) cov = 0.0f;           /* too short to be here */
                uint8_t *o = px + (((size_t)k * FUR_N * FUR_N) + (size_t)y * FUR_N + x) * 2;
                o[0] = (uint8_t)(cov * 255.0f);
                o[1] = (uint8_t)(shade * 255.0f);
            }
        }
    }

    glGenTextures(1, &G.fur_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.fur_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RG8, FUR_N, FUR_N, FUR_SLICES, 0,
                 GL_RG, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    {   GLfloat aniso = 0.0f;
        glGetFloatv(0x84FF, &aniso);
        if (aniso > 1.0f) {
            if (aniso > 16.0f) aniso = 16.0f;
            glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE, aniso);
        }
        while (glGetError() != GL_NO_ERROR) { }
    }
    free(px);
    /* Report what actually got baked. Guessing thresholds against a noise
     * distribution is how the first version came out empty: every slice was
     * clamped to zero coverage and the shells drew nothing at all. */
    for (int k = 0; k < FUR_SLICES; k += FUR_SLICES - 1 > 0 ? FUR_SLICES - 1 : 1) {
        long sum = 0, nz = 0;
        for (int i = 0; i < FUR_N * FUR_N; i++) {
            int c = px[(((size_t)k * FUR_N * FUR_N) + i) * 2];
            sum += c;
            if (c > 8) nz++;
        }
        LOGI("[cuevr] fur slice %d: mean cover %.3f, %.1f%% of texels covered",
             k, (double)sum / (255.0 * FUR_N * FUR_N),
             100.0 * (double)nz / (FUR_N * FUR_N));
    }
    LOGI("[cuevr] baked a %d-slice fur volume (%dx%d, %.1f mm pile)",
         FUR_SLICES, FUR_N, FUR_N, (double)(FUR_PILE * 1000.0f));
}

/* ---- fins ---------------------------------------------------------------- *
 * The other half of shells-and-fins, and the half that actually shows you a
 * hair. Shells alone only read where the pile is seen edge-on: look down into
 * them and eight layers inside three millimetres collapse into one. NVIDIA's
 * sample extrudes fins from silhouette EDGES, which works on a cat and does
 * nothing on a snooker table, because a flat plane has no interior silhouette
 * edges at all — its only edges are its boundary.
 *
 * So the fins are distributed instead of found: a patch of crossed vertical
 * cards, each one a slice of hair standing up out of the cloth. Crossed pairs so
 * there is always a card facing you whichever way you look. This is how grass is
 * drawn, and grass is the same problem — a flat surface, fine strands, and a
 * viewer who is usually looking along it rather than down at it.
 *
 * A patch, not the whole table: at 7 mm spacing the full twelve-foot cloth would
 * be ninety thousand cards, and you can only ever be close to one place at a
 * time. The patch follows your eye, snapped to its own grid so it does not crawl
 * underneath you. */
#define FIN_SPACING 0.007f     /* metres between cards */
#define FIN_HALF    0.22f      /* patch half-extent */
#define FIN_W       0.0075f    /* card width */

static void build_fins(Builder *b) {
    int n = (int)(FIN_HALF * 2.0f / FIN_SPACING);
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < n; i++) {
            float x = -FIN_HALF + ((float)i + 0.5f) * FIN_SPACING;
            float z = -FIN_HALF + ((float)j + 0.5f) * FIN_SPACING;
            /* Jitter so the cards are not a visible lattice. Deterministic. */
            unsigned h = (unsigned)(i * 73856093) ^ (unsigned)(j * 19349663);
            float jx = ((float)((h >> 8) & 255) / 255.0f - 0.5f) * FIN_SPACING * 0.8f;
            float jz = ((float)((h >> 16) & 255) / 255.0f - 0.5f) * FIN_SPACING * 0.8f;
            x += jx; z += jz;
            /* Every pair gets its own ANGLE and its own height. Axis-aligned
             * crosses on a regular grid read as exactly that — a lattice of X
             * shapes — because every card in the patch faces the same two ways and
             * ends at the same height. Rotating and shortening each one
             * independently is what turns a grid of cards into a pile of hair. */
            float ang = (float)((h >> 3) & 1023) / 1023.0f * 3.14159265f;
            float hgt = 0.55f + (float)((h >> 21) & 255) / 255.0f * 0.45f;
            float ca = cosf(ang), sa = sinf(ang);
            for (int k = 0; k < 2; k++) {
                /* the pair is at ang and ang+90 */
                float ux = k ? -sa : ca, uz = k ? ca : sa;
                float dx = ux * FIN_W * 0.5f;
                float dz = uz * FIN_W * 0.5f;
                /* uv.x runs across the card, uv.y up it, so the shader can walk
                 * the fur volume vertically and get a strand profile. */
                int a0 = b_vert(b, x - dx, 0.0f, z - dz, 0, 1, 0, 0.0f, 0.0f);
                int a1 = b_vert(b, x + dx, 0.0f, z + dz, 0, 1, 0, 1.0f, 0.0f);
                int a2 = b_vert(b, x + dx, hgt, z + dz, 0, 1, 0, 1.0f, 1.0f);
                int a3 = b_vert(b, x - dx, hgt, z - dz, 0, 1, 0, 0.0f, 1.0f);
                b_tri(b, a0, a1, a2);
                b_tri(b, a0, a2, a3);
            }
        }
    }
}

/* ---- ball numbers, in Audiowide ------------------------------------------ *
 * cue_render draws them from the handheld's 3x5 bitmap font, which is right for a
 * 128x128 screen and looks like a spreadsheet when the ball is 6 cm from your
 * eye. The letterforms are the same Audiowide the rest of the UI now uses.
 *
 * The trick is knowing WHERE the number is without reproducing cue_render's
 * layout: number_patch() paints its circle in one exact white and its ink in one
 * exact near-black, so a texel coming back as either of those is a texel inside
 * the number patch. That is a reliable marker and it costs nothing.
 *
 * The patch maps the +x pole cap to a unit disc: py = nb.y * 2.30, pz = nb.z *
 * 2.30, inside r <= 1. Same mapping here, so the digit lands exactly where
 * cue_render put its circle. */
#define NUMG   128                 /* glyph raster per ball */
static uint8_t s_numg[16][NUMG * NUMG];
static int     s_numbox[16][4];   /* ink bbox: x0,y0,x1,y1 */
static int     s_numg_ready;

static void bake_ball_numbers(void) {
    if (s_numg_ready) return;
    s_numg_ready = 1;
    static uint16_t tmp[NUMG * NUMG];
    for (int n = 1; n <= 15; n++) {
        memset(tmp, 0, sizeof tmp);
        char txt[4];
        snprintf(txt, sizeof txt, "%d", n);
        /* Centre it. The lg face is 44 px in a 64 box, which leaves the digits
         * filling the circle the way a real ball's do. */
        /* Render big, then record where the INK actually is. Mapping the
         * padded raster onto the ball is what kept the digit small: a font box
         * carries room for ascenders and descenders that no digit uses, so most
         * of what got mapped was empty margin. The bbox is mapped instead, which
         * is what makes the number fill its circle the way a real ball's does. */
        int w = cuevr_text_width(&cuevr_font_xl, txt);
        cuevr_text_draw(tmp, NUMG, NUMG, &cuevr_font_xl, txt,
                        (NUMG - w) / 2, NUMG / 4, 0xFFFF);
        {
            int x0 = NUMG, y0 = NUMG, x1 = -1, y1 = -1;
            for (int yy = 0; yy < NUMG; yy++)
                for (int xx = 0; xx < NUMG; xx++)
                    if (tmp[yy * NUMG + xx]) {
                        if (xx < x0) x0 = xx;
                        if (xx > x1) x1 = xx;
                        if (yy < y0) y0 = yy;
                        if (yy > y1) y1 = yy;
                    }
            if (x1 < x0) { x0 = y0 = 0; x1 = y1 = NUMG - 1; }
            s_numbox[n][0] = x0; s_numbox[n][1] = y0;
            s_numbox[n][2] = x1; s_numbox[n][3] = y1;
        }
        for (int i = 0; i < NUMG * NUMG; i++)
            s_numg[n][i] = (uint8_t)(((tmp[i] >> 5) & 63) * 255 / 63);   /* green = coverage */
    }
}

/* Coverage of ball `id`'s number at pole-cap coordinates (py, pz), or -1 if this
 * texel is not in the digit area at all. */
static float ball_number_cov(uint8_t id, float py, float pz) {
    if (id < 1 || id > 15) return -1.0f;
    const int *bx = s_numbox[id];
    float bw = (float)(bx[2] - bx[0] + 1), bh = (float)(bx[3] - bx[1] + 1);
    /* The digit's height, in disc units where the circle has radius 1 (so the
     * circle's diameter is 2). On a real ball the number stands about a third of
     * the circle's diameter, not the whole of it — 1.05 filled the circle edge to
     * edge and looked like a logo. */
    float wh = 0.72f;
    float ww = wh * (bw / bh);
    float gu = (pz + ww * 0.5f) / ww;
    float gv = (wh * 0.5f - py) / wh;
    if (gu < 0.0f || gu > 1.0f || gv < 0.0f || gv > 1.0f) return 0.0f;
    float fx = (float)bx[0] + gu * (bw - 1.0f);
    float fy = (float)bx[1] + gv * (bh - 1.0f);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < NUMG ? x0 + 1 : x0, y1 = y0 + 1 < NUMG ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;
    const uint8_t *g = s_numg[id];
    float a = g[y0 * NUMG + x0] / 255.0f, b = g[y0 * NUMG + x1] / 255.0f;
    float c = g[y1 * NUMG + x0] / 255.0f, d = g[y1 * NUMG + x1] / 255.0f;
    float t0 = a + (b - a) * tx, t1 = c + (d - c) * tx;
    return t0 + (t1 - t0) * ty;
}

static void bake_ball_atlas(void) {
    bake_ball_numbers();
    if (G.ball_tex) { glDeleteTextures(1, &G.ball_tex); G.ball_tex = 0; }
    uint16_t *px = malloc((size_t)BTEX_W * BTEX_H * BTEX_IDS * sizeof(uint16_t));
    if (!px) { LOGI("[cuevr] no memory for the ball atlas"); return; }
    for (int id = 0; id < BTEX_IDS; id++) {
        uint16_t *slice = px + (size_t)id * BTEX_W * BTEX_H;
        for (int y = 0; y < BTEX_H; y++) {
            float phi = ((float)y + 0.5f) / BTEX_H * PI;          /* 0 = +Y pole */
            for (int x = 0; x < BTEX_W; x++) {
                float th = (((float)x + 0.5f) / BTEX_W - 0.5f) * 2.0f * PI;
                Vec3 nb;
                nb.x = sinf(phi) * cosf(th);
                nb.y = cosf(phi);
                nb.z = sinf(phi) * sinf(th);
                uint16_t c = cue_render_ball_texel((uint8_t)id, nb);
                /* A texel that came back as the patch's white or its ink is a
                 * texel inside the number circle — so re-draw the digit there in
                 * Audiowide instead of the 3x5 font's version of it. */
                if (c == RGB565C(245, 245, 245) || c == RGB565C(15, 15, 18)) {
                    float py = nb.y * 2.30f, pz = nb.z * 2.30f;
                    float cov = ball_number_cov((uint8_t)id, py, pz);
                    if (cov >= 0.0f) {
                        const int wr = 245, wg = 245, wb = 245;
                        const int ir = 15,  ig = 15,  ib = 18;
                        int r = (int)(wr + (ir - wr) * cov);
                        int g2 = (int)(wg + (ig - wg) * cov);
                        int b2 = (int)(wb + (ib - wb) * cov);
                        c = (uint16_t)(((r >> 3) << 11) | ((g2 >> 2) << 5) | (b2 >> 3));
                    }
                }
                slice[y * BTEX_W + x] = c;
            }
        }
    }
    /* CUEVR_DUMP_BALLS=<id>: write the baked slice as a PPM. Looking at the
     * texture itself is the only way to tell a bake problem from a shading one. */
    if (getenv("CUEVR_DUMP_BALLS")) {
        int id = atoi(getenv("CUEVR_DUMP_BALLS"));
        if (id >= 0 && id < BTEX_IDS) {
            FILE *fp = fopen("/tmp/ball_slice.ppm", "wb");
            if (fp) {
                fprintf(fp, "P6\n%d %d\n255\n", BTEX_W, BTEX_H);
                const uint16_t *sl = px + (size_t)id * BTEX_W * BTEX_H;
                for (int i = 0; i < BTEX_W * BTEX_H; i++) {
                    uint16_t c = sl[i];
                    unsigned char rgb[3] = {
                        (unsigned char)(((c >> 11) & 0x1F) * 255 / 31),
                        (unsigned char)(((c >> 5) & 0x3F) * 255 / 63),
                        (unsigned char)((c & 0x1F) * 255 / 31) };
                    fwrite(rgb, 1, 3, fp);
                }
                fclose(fp);
                LOGI("[cuevr] dumped ball %d to /tmp/ball_slice.ppm", id);
            }
        }
    }

    glGenTextures(1, &G.ball_tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.ball_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB565, BTEX_W, BTEX_H, BTEX_IDS, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
    /* Mipmaps matter as much as the resolution does. A 256x128 ball seen across
     * a 12 ft table covers a handful of pixels, and point-sampling a sharp
     * number or stripe at that rate crawls and sparkles as the ball rolls.
     * Trilinear picks the level that matches the footprint. */
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, getenv("CUEVR_NOMIP") ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
    /* Do not let it reach the coarsest levels. An equirect map smears anything
     * sitting on a pole across every longitude — the cue ball's two pole dots
     * become full-width red bands in the texture — and at a sphere's SILHOUETTE
     * the u derivative explodes, because a whole revolution of longitude
     * compresses into a couple of pixels. The GPU then picks a mip near the top
     * of the chain, and those levels average the pole bands into everything,
     * which painted a red ring right around the ball: the "strange equator".
     *
     * Level 4 is 16x8, still coarse enough to stop distant balls shimmering and
     * still fine enough that a pole band does not contaminate the equator. The
     * residual rim aliasing is what MSAA is for.
     *
     * The proper fix is to stop using an equirect map for a sphere at all — a
     * cube map has no poles to smear and filters correctly across its seams. */
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 4);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* Wrap in u so the seam behind the ball closes. v is clamped, but a layer
     * has no neighbour to bleed from now, so this is only about the poles. */
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    {   /* Anisotropy if the driver has it: a ball is a sphere, so its texture
         * footprint is stretched badly near the silhouette. */
        GLfloat aniso = 0.0f;
        glGetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY_EXT */, &aniso);
        if (aniso > 1.0f && !getenv("CUEVR_NOMIP")) {
            if (aniso > 8.0f) aniso = 8.0f;
            glTexParameterf(GL_TEXTURE_2D_ARRAY, 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */, aniso);
        }
        while (glGetError() != GL_NO_ERROR) { }   /* absent extension is fine */
    }
    free(px);
    LOGI("[cuevr] baked %d ball surfaces from cue_render", BTEX_IDS);
}

/* ---- init --------------------------------------------------------------- */

int cuevr_render_init(const CueTable *t, const CueWorld *w, int target_is_srgb) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) return -1;
    G.prog = glCreateProgram();
    glAttachShader(G.prog, vs);
    glAttachShader(G.prog, fs);
    glLinkProgram(G.prog);
    GLint ok = 0;
    glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
    if (!ok) { char l[1024]; glGetProgramInfoLog(G.prog, sizeof l, NULL, l); LOGI("[cuevr] link: %s", l); return -1; }
    glDeleteShader(vs); glDeleteShader(fs);

    G.u_mvp      = glGetUniformLocation(G.prog, "u_mvp");
    G.u_model    = glGetUniformLocation(G.prog, "u_model");
    G.u_tex      = glGetUniformLocation(G.prog, "u_tex");
    G.u_mode     = glGetUniformLocation(G.prog, "u_mode");
    G.u_encode   = glGetUniformLocation(G.prog, "u_encode");
    G.u_colour   = glGetUniformLocation(G.prog, "u_colour");
    G.u_colour2  = glGetUniformLocation(G.prog, "u_colour2");
    G.u_light    = glGetUniformLocation(G.prog, "u_light");
    G.u_ballslice  = glGetUniformLocation(G.prog, "u_ballslice");
    G.u_balls      = glGetUniformLocation(G.prog, "u_balls");
    /* Array uniforms: ask for element 0 by name. Querying the bare array name
     * is legal but not honoured by every driver, and a silent -1 here sets no
     * lamps at all — which looks exactly like a polished ball with no lamps
     * above it. Every location is checked below for the same reason. */
    G.u_lampC      = glGetUniformLocation(G.prog, "u_lampC[0]");
    G.u_lampX      = glGetUniformLocation(G.prog, "u_lampX[0]");
    G.u_lampZ      = glGetUniformLocation(G.prog, "u_lampZ[0]");
    G.u_nlamp      = glGetUniformLocation(G.prog, "u_nlamp");
    G.u_eye        = glGetUniformLocation(G.prog, "u_eye");
    G.u_clothsh    = glGetUniformLocation(G.prog, "u_clothsh");
    G.u_cloth      = glGetUniformLocation(G.prog, "u_cloth");
    G.u_fur        = glGetUniformLocation(G.prog, "u_fur");
    G.u_nap        = glGetUniformLocation(G.prog, "u_nap");
    G.u_feltspan   = glGetUniformLocation(G.prog, "u_feltspan");
    G.u_furslice   = glGetUniformLocation(G.prog, "u_furslice");
    G.u_furslices  = glGetUniformLocation(G.prog, "u_furslices");
    G.u_furdbg     = glGetUniformLocation(G.prog, "u_furdbg");
    G.u_cshaft     = glGetUniformLocation(G.prog, "u_cshaft");
    G.u_csplice    = glGetUniformLocation(G.prog, "u_csplice");
    G.u_caccent    = glGetUniformLocation(G.prog, "u_caccent");
    G.u_cbutt      = glGetUniformLocation(G.prog, "u_cbutt");
    G.u_cburr      = glGetUniformLocation(G.prog, "u_cburr");
    G.u_cflash     = glGetUniformLocation(G.prog, "u_cflash");
    G.u_shell      = glGetUniformLocation(G.prog, "u_shell");
    G.u_markc      = glGetUniformLocation(G.prog, "u_markc");
    G.u_baulk      = glGetUniformLocation(G.prog, "u_baulk");
    G.u_drad       = glGetUniformLocation(G.prog, "u_drad");
    G.u_linew      = glGetUniformLocation(G.prog, "u_linew");
    G.u_spotr      = glGetUniformLocation(G.prog, "u_spotr");
    G.u_nspot      = glGetUniformLocation(G.prog, "u_nspot");
    G.u_spots      = glGetUniformLocation(G.prog, "u_spots");
    if (G.u_lampC < 0 || G.u_eye < 0 || G.u_clothsh < 0 || G.u_light < 0)
        LOGI("[cuevr] WARNING: lighting uniforms missing (lampC %d eye %d clothsh %d light %d)",
             G.u_lampC, G.u_eye, G.u_clothsh, G.u_light);
    G.encode = target_is_srgb ? 0 : 1;
    G.tab = *t;

    cuevr_render_set_table(t, w);

    Builder b;
    b_init(&b, 4096, 12288);
    build_sphere(&b, 24, 16);
    mesh_upload(&G.ball, &b);
    b_free(&b);

    b_init(&b, 20 * 64 + 64, 20 * 64 * 6 + 64);
    build_cue(&b, 20, 48);
    mesh_upload(&G.cue, &b);
    b_free(&b);

    /* The real controllers, from Meta's own Touch Pro models (tools/stl2ctrl.py
     * welds and decimates them). They are here so you can see where your hands are
     * and line the cue up against a contact point that makes sense — not to be
     * admired — so 3000 triangles each after welding is ample and the block proxies
     * that stood in for them are gone. */
    {
        Builder cb;
        b_init(&cb, CTRL_LEFT_NV + 4, CTRL_LEFT_NI + 8);
        for (int i = 0; i < CTRL_LEFT_NV; i++) {
            const int16_t *p = ctrl_left_pos + i * 3;
            const int8_t  *n = ctrl_left_nrm + i * 3;
            b_vert(&cb, p[0] * CTRL_LEFT_SCALE, p[1] * CTRL_LEFT_SCALE, p[2] * CTRL_LEFT_SCALE,
                   n[0] / 127.0f, n[1] / 127.0f, n[2] / 127.0f, 0.0f, 0.0f);
        }
        for (int i = 0; i < CTRL_LEFT_NI; i += 3)
            b_tri(&cb, ctrl_left_idx[i], ctrl_left_idx[i+1], ctrl_left_idx[i+2]);
        mesh_upload(&G.ctrl[0], &cb);
        b_free(&cb);

        b_init(&cb, CTRL_RIGHT_NV + 4, CTRL_RIGHT_NI + 8);
        for (int i = 0; i < CTRL_RIGHT_NV; i++) {
            const int16_t *p = ctrl_right_pos + i * 3;
            const int8_t  *n = ctrl_right_nrm + i * 3;
            b_vert(&cb, p[0] * CTRL_RIGHT_SCALE, p[1] * CTRL_RIGHT_SCALE, p[2] * CTRL_RIGHT_SCALE,
                   n[0] / 127.0f, n[1] / 127.0f, n[2] / 127.0f, 0.0f, 0.0f);
        }
        for (int i = 0; i < CTRL_RIGHT_NI; i += 3)
            b_tri(&cb, ctrl_right_idx[i], ctrl_right_idx[i+1], ctrl_right_idx[i+2]);
        mesh_upload(&G.ctrl[1], &cb);
        b_free(&cb);
    }

    /* Controller proxy: a small block, tapered like a grip. */
    b_init(&b, 64, 96);
    {
        const float w = 0.021f, h = 0.030f, d = 0.052f;
        const float f[3] = {0,0,1}, bk[3] = {0,0,-1}, l[3] = {-1,0,0},
                    r[3] = {1,0,0}, u[3] = {0,1,0}, dn[3] = {0,-1,0};
        float A[3]={-w,-h,-d}, B[3]={w,-h,-d}, C[3]={w*0.75f,h,-d*0.55f}, D[3]={-w*0.75f,h,-d*0.55f};
        float E[3]={-w,-h, d}, F[3]={w,-h, d}, G[3]={w*0.75f,h, d*0.35f}, H[3]={-w*0.75f,h, d*0.35f};
        b_face(&b, A,B,C,D, bk); b_face(&b, E,F,G,H, f);
        b_face(&b, A,E,H,D, l);  b_face(&b, B,F,G,C, r);
        b_face(&b, D,C,G,H, u);  b_face(&b, A,B,F,E, dn);
    }
    mesh_upload(&G.grip, &b);
    b_free(&b);

    b_init(&b, 8, 12);
    {   float p0[3]={-0.5f,-0.5f,0}, p1[3]={0.5f,-0.5f,0}, p2[3]={0.5f,0.5f,0}, p3[3]={-0.5f,0.5f,0};
        float n[3]={0,0,1};
        int a = b_vert(&b, p0[0],p0[1],p0[2], n[0],n[1],n[2], 0,1);
        int c = b_vert(&b, p1[0],p1[1],p1[2], n[0],n[1],n[2], 1,1);
        int d = b_vert(&b, p2[0],p2[1],p2[2], n[0],n[1],n[2], 1,0);
        int e = b_vert(&b, p3[0],p3[1],p3[2], n[0],n[1],n[2], 0,0);
        b_quad(&b, a, c, d, e);
    }
    mesh_upload(&G.quad, &b);
    b_free(&b);

    b_init(&b, 8, 12);
    {   float s = 8.0f;
        float p0[3]={-s,0,-s}, p1[3]={s,0,-s}, p2[3]={s,0,s}, p3[3]={-s,0,s};
        float n[3]={0,1,0};
        b_face(&b, p0, p1, p2, p3, n);
    }
    mesh_upload(&G.floor, &b);
    b_free(&b);

    glGenTextures(1, &G.hud_tex);
    glBindTexture(GL_TEXTURE_2D, G.hud_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, CUEVR_HUD_W, CUEVR_HUD_H, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    G.ready = 1;
    LOGI("[cuevr] table %.2f x %.2f m, %d cushion segs, %d pockets",
         t->half_len * 2, t->half_wid * 2, w->nseg, w->npocket);
    return 0;
}

void cuevr_render_set_table(const CueTable *t, const CueWorld *w) {
    G.tab = *t;
    mesh_free(&G.table); mesh_free(&G.lips);

    /* cue_render keeps its two big buffers outside itself (on the handheld they
     * live in the engine arena); here they are just malloc'd once. */
    if (!G.tab_buf) {
        G.tab_buf  = malloc(cue_render_tab_bytes());
        G.stri_buf = malloc(cue_render_stri_bytes());
        if (!G.tab_buf || !G.stri_buf) { LOGI("[cuevr] out of memory for the table mesh"); return; }
        cue_render_set_buffers(G.tab_buf, G.stri_buf);
    }
    cue_render_build_table(t, w);

    const CueTri *tris = NULL;
    int bed = 0, lip = 0;
    int n = cue_render_table_tris(&tris, &bed, &lip);
    if (!tris || n <= 0) { LOGI("[cuevr] the table mesh came back empty"); return; }

    /* The flat cloth goes into its own mesh: it is the one surface whose
     * markings are computed rather than baked, so it needs its own shader. */
    Builder b;
    b_init(&b, bed * 3 + 8, bed * 3 + 8);
    build_from_cue_render(&b, tris, 0, bed);
    mesh_upload(&G.bed, &b);
    b_free(&b);

    b_init(&b, (lip - bed) * 3 + 8, (lip - bed) * 3 + 8);
    build_from_cue_render(&b, tris, bed, lip);
    mesh_upload(&G.table, &b);
    b_free(&b);

    int nlip = n - lip;
    if (nlip > 0) {
        b_init(&b, nlip * 3 + 8, nlip * 3 + 8);
        build_from_cue_render(&b, tris, lip, n);
        mesh_upload(&G.lips, &b);
        b_free(&b);
    }
    bake_nap();
    bake_fur();
    {   /* One patch, built once, moved to follow the eye. */
        int n = (int)(FIN_HALF * 2.0f / FIN_SPACING);
        Builder fb;
        b_init(&fb, n * n * 8 + 8, n * n * 12 + 8);
        build_fins(&fb);
        mesh_upload(&G.fins, &fb);
        LOGI("[cuevr] fin patch: %d cards, %d tris", n * n * 2, fb.ni / 3);
        b_free(&fb);
    }
    bake_ball_atlas();

    /* The frame is generated separately and drawn separately — nothing above
     * this line knows it exists, and swapping designs cannot disturb the bed,
     * the cushions or the pockets. */
    mesh_free(&G.frame);
    {
        int cv, ci;
        cuevr_frame_capacity(&cv, &ci);
        CueVrFrameMesh fm;
        memset(&fm, 0, sizeof fm);
        fm.v = malloc(sizeof(CueVrFrameVtx) * (size_t)cv);
        fm.idx = malloc(sizeof(uint16_t) * (size_t)ci);
        fm.cap_v = cv; fm.cap_i = ci;
        if (fm.v && fm.idx) {
            cuevr_frame_build(G.frame_sel, &fm, t);
            if (fm.overflow) LOGI("[cuevr] frame '%s' ran out of room",
                                  CUEVR_FRAMES[G.frame_sel].name);
            /* CueVrFrameVtx and the renderer's Vtx are the same layout, so this
             * goes straight to the GPU. */
            Builder fb;
            fb.v = (Vtx *)fm.v; fb.i = fm.idx;
            fb.nv = fm.nv; fb.ni = fm.ni; fb.cap_v = cv; fb.cap_i = ci;
            mesh_upload(&G.frame, &fb);
            LOGI("[cuevr] frame '%s': %d tris", CUEVR_FRAMES[G.frame_sel].name, fm.ni / 3);
        }
        free(fm.v); free(fm.idx);
    }

    LOGI("[cuevr] table mesh %d tris (%d bed, %d lip), balls re-baked (%s)",
         n, bed, nlip, t->is_snooker ? "snooker" : "pool");
}

void cuevr_render_shutdown(void) {
    if (!G.ready) return;
    mesh_free(&G.ctrl[0]); mesh_free(&G.ctrl[1]);
    mesh_free(&G.fins);
    mesh_free(&G.bed);
    mesh_free(&G.grip);
    mesh_free(&G.table); mesh_free(&G.lips); mesh_free(&G.frame); mesh_free(&G.ball);
    mesh_free(&G.cue); mesh_free(&G.quad); mesh_free(&G.floor);
    if (G.nap_tex) glDeleteTextures(1, &G.nap_tex);
    if (G.fur_tex) glDeleteTextures(1, &G.fur_tex);
    glDeleteTextures(1, &G.hud_tex);
    glDeleteTextures(1, &G.ball_tex);
    free(G.tab_buf); free(G.stri_buf); G.tab_buf = G.stri_buf = NULL;
    glDeleteProgram(G.prog);
    G.ready = 0;
}

void cuevr_render_hud(const uint16_t *px) {
    if (!G.ready) return;
    glBindTexture(GL_TEXTURE_2D, G.hud_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, CUEVR_HUD_W, CUEVR_HUD_H,
                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, px);
}

/* ---- drawing ------------------------------------------------------------ */

/* Two view-projections, because multiview draws both eyes in one pass. The
 * non-multiview path fills only slot 0 and the shader's VIEW_ID is 0, so the same
 * code serves both. */
static float VP[2][16];
static int   VP_n = 1;

static void set_model(const float *m) {
    float mvp[2][16];
    for (int v = 0; v < VP_n; v++) mm4_mul(mvp[v], VP[v], m);
    glUniformMatrix4fv(G.u_mvp, VP_n, GL_FALSE, mvp[0]);
    glUniformMatrix4fv(G.u_model, 1, GL_FALSE, m);
}

static void draw(const Mesh *m) {
    if (!m->n) return;
    glBindVertexArray(m->vao);
    glDrawElements(GL_TRIANGLES, m->n, GL_UNSIGNED_SHORT, 0);
}

static void colour(float r, float g, float b, float a) { glUniform4f(G.u_colour, r, g, b, a); }

/* rgb565 -> 0..1 floats, so the table's own palette drives the 3D colours too */
static void colour565(uint16_t c, float mul) {
    float r = ((c >> 11) & 31) / 31.0f, g = ((c >> 5) & 63) / 63.0f, b = (c & 31) / 31.0f;
    colour(r * mul, g * mul, b * mul, 1.0f);
}

/* Multiview entry point: the same renderer, told there are two views. */
void cuevr_render_views(const float *view2, const float *proj2,
                        const CueVrScene *s, int draw_room) {
    VP_n = 2;
    cuevr_render_eye(view2, proj2, s, draw_room);
    VP_n = 1;
}

void cuevr_render_eye(const float *view, const float *proj,
                      const CueVrScene *s, int draw_room)
{
    if (!G.ready) return;
    for (int v = 0; v < VP_n; v++) mm4_mul(VP[v], proj + v * 16, view + v * 16);

    glUseProgram(G.prog);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(G.u_tex, 0);
    glUniform1i(G.u_encode, G.encode);

    /* The lighting rig: shades hanging over the table, as a real room has.
     *
     * The handheld carries four lamp DIRECTIONS and tests a half-vector,
     * because at 128x128 a reflection is one white pixel and a direction is all
     * you need to place it. Here the reflection is a shape, so the lamps have
     * to be objects: rectangles at a height above the cloth, which the shader
     * intersects with the reflected view ray. They hang over the table, so they
     * are built in table space and carried out into the room with it.
     */
    /* Sized and hung like the real thing. A snooker light is a long low bar of
     * big shades — and the size of the reflection is the size of the SOURCE:
     * a small lamp high up reflects in a 26 mm ball as a speck, which is what
     * the first attempt at this looked like. Low and wide gives the long bright
     * streaks you actually see down a table. */
    const float LAMP_H  = 0.62f;      /* above the cloth */
    const float LAMP_HX = 0.30f;      /* half the shade, along the table */
    const float LAMP_HZ = 0.17f;      /* and across it */
    float cy = cosf(s->place->yaw), sy = sinf(s->place->yaw);
    int nlamp = (int)(G.tab.half_len * 2.0f / 0.85f + 0.5f);
    if (nlamp < 2) nlamp = 2;
    if (nlamp > 4) nlamp = 4;
    {
        float cen[12], axx[12], axz[12];
        float L = G.tab.half_len * 2.0f;
        for (int i = 0; i < nlamp; i++) {
            float tx = ((float)i + 0.5f) / nlamp * L - L * 0.5f;
            cen[i*3+0] = s->place->pos.x + tx * cy;
            cen[i*3+1] = s->place->pos.y + LAMP_H;
            cen[i*3+2] = s->place->pos.z + tx * sy;
            axx[i*3+0] = LAMP_HX * cy;  axx[i*3+1] = 0.0f; axx[i*3+2] = LAMP_HX * sy;
            axz[i*3+0] = -LAMP_HZ * sy; axz[i*3+1] = 0.0f; axz[i*3+2] = LAMP_HZ * cy;
        }
        glUniform3fv(G.u_lampC, nlamp, cen);
        glUniform3fv(G.u_lampX, nlamp, axx);
        glUniform3fv(G.u_lampZ, nlamp, axz);
        glUniform1i(G.u_nlamp, nlamp);
    }

    /* The eye, recovered from the view matrix: its rows are the camera basis
     * and its translation is that basis applied to -eye, so undo it. */
    MoteVrV3 eye;
    {
        float ep[2][3];
        for (int v = 0; v < VP_n; v++) {
            const float *vw = view + v * 16;
            ep[v][0] = -(vw[12]*vw[0] + vw[13]*vw[1] + vw[14]*vw[2]);
            ep[v][1] = -(vw[12]*vw[4] + vw[13]*vw[5] + vw[14]*vw[6]);
            ep[v][2] = -(vw[12]*vw[8] + vw[13]*vw[9] + vw[14]*vw[10]);
        }
        glUniform3fv(G.u_eye, VP_n, ep[0]);
        /* Shading that wants a single eye — the fur gate, the scoreboard sizing —
         * uses the left, which is within 32 mm of the right. */
        eye = mv3(ep[0][0], ep[0][1], ep[0][2]);
    }

    /* The key light stays the handheld's: nearly overhead, rotated with the
     * table so it is over the cloth and not over your kitchen. */
    {
        MoteVrV3 k = mv3_norm(mv3(0.10f * cy - 0.20f * sy, 0.975f,
                                  0.10f * sy + 0.20f * cy));
        glUniform3f(G.u_light, k.x, k.y, k.z);
    }

    {   /* cloth bounce: the cloth's own colour at 0.42, as shade565 gives it */
        float r = ((G.tab.cloth >> 11) & 31) / 31.0f;
        float g = ((G.tab.cloth >> 5) & 63) / 63.0f;
        float b = (G.tab.cloth & 31) / 31.0f;
        glUniform3f(G.u_clothsh, r * 0.42f, g * 0.42f, b * 0.42f);
    }

    /* The table's own transform: yaw about vertical, then out into the room. */
    float T[16];
    {
        MoteVrPose pose;
        pose.p = s->place->pos;
        pose.q = mq_axis_angle(mv3(0, 1, 0), s->place->yaw);
        mm4_from_pose(T, pose, 1.0f);
    }

    if (draw_room) {
        glUniform1i(G.u_mode, 3);
        colour(0.35f, 0.42f, 0.55f, 1.0f);
        float F[16];
        mm4_identity(F);
        F[13] = s->place->pos.y - s->place->height;   /* the room's floor */
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        set_model(F);
        draw(&G.floor);
        glEnable(GL_CULL_FACE);
        glDisable(GL_BLEND);
    }

    /* The frame first: it is underneath everything and honestly wound, so it
     * keeps backface culling. */
    if (G.frame.n) {
        glUniform1i(G.u_mode, 7);
        set_model(T);
        draw(&G.frame);
    }

    /* No backface culling on the table. cue_render's mesh is authored for a
     * software rasteriser that does not cull — the cloth fan, the cushion
     * faces and the pocket voids are wound for shading, not for a front-face
     * convention — so culling it silently drops the bed and half the rails.
     * Depth sorts it correctly regardless. */
    glDisable(GL_CULL_FACE);
    glUniform1i(G.u_mode, 4);          /* vertex colours, as authored */
    set_model(T);
    draw(&G.table);

    if (getenv("CUEVR_NOSHADOW")) goto skip_shadows;
    /* ---- the cloth, with its chalk ---- *
     * The spots come from the table itself rather than from anything invented
     * here: snooker's six colours sit on the D ends, the centre of the D, and
     * the blue, pink and black spots along the middle; a pool table keeps its
     * centre and black spots. Same numbers the rack is laid out from, so the
     * chalk is under the ball rather than near it. */
    if (G.bed.n) {
        const CueTable *tb = &G.tab;
        float sp[16]; int ns = 0;
        if (tb->is_snooker) {
            sp[ns*2] = tb->baulk_x; sp[ns*2+1] = -tb->d_radius; ns++;   /* green  */
            sp[ns*2] = tb->baulk_x; sp[ns*2+1] =  tb->d_radius; ns++;   /* yellow */
            sp[ns*2] = tb->baulk_x; sp[ns*2+1] =  0.0f;         ns++;   /* brown  */
            sp[ns*2] = tb->blue_x;  sp[ns*2+1] =  0.0f;         ns++;
            sp[ns*2] = tb->pink_x;  sp[ns*2+1] =  0.0f;         ns++;
            sp[ns*2] = tb->black_x; sp[ns*2+1] =  0.0f;         ns++;
        } else {
            sp[ns*2] = 0.0f;        sp[ns*2+1] = 0.0f;          ns++;   /* centre */
            sp[ns*2] = tb->black_x; sp[ns*2+1] = 0.0f;          ns++;
        }
        {
            float r = ((tb->cloth >> 11) & 31) / 31.0f;
            float g = ((tb->cloth >> 5) & 63) / 63.0f;
            float bb = (tb->cloth & 31) / 31.0f;
            glUniform3f(G.u_cloth, r, g, bb);
        }
        {
            float r = ((tb->spot >> 11) & 31) / 31.0f;
            float g = ((tb->spot >> 5) & 63) / 63.0f;
            float bb = (tb->spot & 31) / 31.0f;
            glUniform3f(G.u_markc, r, g, bb);
        }
        /* The fur volume on unit 2. CUEVR_FUR scales the tile, which is the only
         * knob: bigger means coarser, more visible hairs. */
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, G.fur_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(G.u_fur, 2);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, G.nap_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(G.u_nap, 3);
        /* CUEVR_FUR scales the WHOLE pile — tile span and height together — so
         * the preview can zoom into a 3 mm pile it otherwise cannot resolve.
         * Scaling only the tile made hairs 6 mm wide and 3 mm tall, which is a
         * blob, not a hair, and told me nothing. */
        G.fur_scale = getenv("CUEVR_FUR") ? (float)atof(getenv("CUEVR_FUR")) : 1.0f;
        glUniform1f(G.u_feltspan, FUR_SPAN * G.fur_scale);
        glUniform1f(G.u_furslices, (float)FUR_SLICES);
        glUniform1f(G.u_furdbg, getenv("CUEVR_FURDBG") ? 1.0f : 0.0f);
        {   const CueVrCueDesign *cd = &CUE_RACK[s_cue_sel];
            glUniform3fv(G.u_cshaft, 1, cd->shaft);
            glUniform3fv(G.u_csplice, 1, cd->splice);
            glUniform3fv(G.u_caccent, 1, cd->accent);
            glUniform3fv(G.u_cbutt, 1, cd->butt);
            glUniform3fv(G.u_cburr, 1, cd->burr);
            glUniform1f(G.u_cflash, (float)cd->flash);
        }
        glUniform1f(G.u_baulk, tb->baulk_x);
        glUniform1f(G.u_drad,  tb->d_radius);
        glUniform1f(G.u_linew, 0.0016f);      /* a chalk line is ~3 mm wide */
        glUniform1f(G.u_spotr, 0.0040f);      /* and a spot ~8 mm across */
        glUniform1i(G.u_nspot, ns);
        glUniform2fv(G.u_spots, ns, sp);
        glUniform1i(G.u_mode, 8);
        glUniform1f(G.u_shell, 0.0f);
        set_model(T);
        draw(&G.bed);

        /* No shell passes.
         *
         * They were eight extra draws of the whole cloth, they were invisible at
         * every distance anyone actually plays from, and they were actively
         * damaging the chalk: the markings live on the backing, so eight layers of
         * pile drawn over them washed the baulk line out wherever the pile was
         * densest. Making every shell carry the chalk fixed the fading and left
         * the line grainy and degraded instead.
         *
         * The reference photograph is the argument for dropping them. A real cloth
         * close up has NO resolvable strands — it has a fine even nap. That is a
         * detail texture, and one mipmapped pass gives it honestly, cannot alias,
         * and cannot touch the chalk. Shells earn their cost on a cat, not on a
         * flat sheet of baize. */
    }

    /* ---- ball shadows on the cloth ---- *
     * The handheld drops a soft decal under every ball. Without them the balls
     * hover a centimetre above the cloth and nothing on the table feels like it
     * is resting on anything. */
    {
        glEnable(GL_BLEND);
        /* Colour blends; ALPHA IS LEFT ALONE.
         *
         * A plain glBlendFunc applies to the alpha channel too, so a shadow at
         * half alpha drops the framebuffer's alpha from 1 to 0.75 — and that
         * alpha is exactly what the Quest compositor uses to blend this layer
         * over the passthrough camera feed. The shadows were not dark blobs on
         * the cloth, they were holes punched through it: you could see the real
         * room, and the real table you had matched the height to, straight
         * through the baize. Anything translucent drawn into a passthrough layer
         * has to preserve destination alpha. */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glUniform1i(G.u_mode, 6);
        /* Darken toward the cloth's own shadowed tone rather than toward black:
         * a ball on baize does not cast an inky disc. */
        glUniform4f(G.u_colour, 0.0f, 0.0f, 0.0f, 1.0f);
        float rad = G.tab.R * 1.55f;
        for (int i = 0; i < s->nballs; i++) {
            const CueBall *bl = &s->balls[i];
            if (!bl->on) continue;
            float local[16], model[16], rot[16];
            mm4_identity(local);
            local[0] = rad * 2.0f; local[5] = rad * 2.0f;
            mm4_identity(rot);
            rot[5] = 0.0f; rot[6] = -1.0f; rot[9] = 1.0f; rot[10] = 0.0f;  /* lie it flat */
            mm4_mul(local, rot, local);
            local[12] = bl->pos.x; local[13] = 0.0015f; local[14] = bl->pos.z;
            mm4_mul(model, T, local);
            set_model(model);
            draw(&G.quad);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glEnable(GL_CULL_FACE);
    }

skip_shadows:
    /* ---- balls ---- */
    glUniform1i(G.u_mode, 1);
    /* The ball array lives on unit 1 for the whole pass; u_tex keeps unit 0. */
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, G.ball_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(G.u_balls, 1);
    for (int i = 0; i < s->nballs; i++) {
        const CueBall *bl = &s->balls[i];
        if (!bl->on) continue;
        int slice = bl->id < BTEX_IDS ? bl->id : 0;
        glUniform1f(G.u_ballslice, (float)slice);

        /* model = table * translate(ball) * orient * scale(R) */
        float B[16], M[16];
        const Mat3 *o = &bl->orient;
        float R = G.tab.R;
        B[0]=o->r[0].x*R; B[1]=o->r[0].y*R; B[2]=o->r[0].z*R; B[3]=0;
        B[4]=o->r[1].x*R; B[5]=o->r[1].y*R; B[6]=o->r[1].z*R; B[7]=0;
        B[8]=o->r[2].x*R; B[9]=o->r[2].y*R; B[10]=o->r[2].z*R; B[11]=0;
        B[12]=bl->pos.x; B[13]=bl->pos.y; B[14]=bl->pos.z; B[15]=1;
        mm4_mul(M, T, B);
        set_model(M);
        draw(&G.ball);
    }

    /* The pocket drop lips last, with depth writes off — the handheld's own
     * order, so a ball resting in a pocket covers the lip rather than the lip
     * drawing across it. */
    if (G.lips.n) {
        glDepthMask(GL_FALSE);
        glUniform1i(G.u_mode, 4);
        set_model(T);
        draw(&G.lips);
        glDepthMask(GL_TRUE);
    }
    glEnable(GL_CULL_FACE);

    /* ---- the cue ---- */
    if (s->cue_visible) {
        glUniform1i(G.u_mode, 5);
        /* The one piece of aim feedback there is: the ferrule goes green when
         * the cue line actually meets the ball. No aim line, no ghost ball —
         * but you should not have to guess whether you are even on it. */
        glUniform4f(G.u_colour, 1, 1, 1, s->cue_on_ball ? 1.0f : 0.0f);
        /* The mesh is a real cue, 1.45 m with y=0 at the tip. So put the tip
         * where the cue line meets the ball and run +Y back along the shaft —
         * past your hands and out behind you, as a real cue does. It is NOT
         * stretched between the controllers: a cue is a fixed length, and
         * making it rubbery is the fastest way to stop believing in it. */
        MoteVrV3 d = mv3_sub(s->cue_butt, s->cue_tip);
        float len = mv3_len(d);
        if (len > 0.02f) {
            MoteVrV3 u = mv3_scale(d, 1.0f / len);
            MoteVrV3 up = mv3(0, 1, 0);
            MoteVrV3 ax = mv3_cross(up, u);
            float s_ = mv3_len(ax), c_ = mv3_dot(up, u);
            MoteVrQ q = (s_ < 1e-5f)
                ? (c_ > 0.0f ? mq_ident() : mq_axis_angle(mv3(1,0,0), PI))
                : mq_axis_angle(ax, atan2f(s_, c_));
            MoteVrPose cp; cp.p = s->cue_tip; cp.q = q;
            float M[16];
            mm4_from_pose(M, cp, 1.0f);
            set_model(M);
            draw(&G.cue);
        }
    }

    /* CUEVR_CTRLAXES: draw the LEFT controller at the table centre, unrotated, with
     * a stick along each of its own axes — red = +x, green = +y, blue = +z. Seven
     * attempts at the model-to-grip matrix were spent guessing which model axis is
     * the handle and which way it points, from bounding-box numbers. This answers it
     * outright, and should have been the first thing built. */
    if (getenv("CUEVR_CTRLAXES") && G.ctrl[0].n) {
        float T2[16], Sm[16], M2[16];
        /* 40 cm in front of the eye, so it is always in shot whatever the camera
         * is pointed at — the table is not at the world origin and the first
         * version of this put the model somewhere off screen. */
        mm4_identity(T2);
        {
            /* the view matrix's third ROW is the camera's backward axis */
            MoteVrV3 fwd = mv3(-view[2], -view[6], -view[10]);
            float l = mv3_len(fwd);
            if (l > 1e-4f) fwd = mv3_scale(fwd, 1.0f / l);
            T2[12] = eye.x + fwd.x * 0.40f;
            T2[13] = eye.y + fwd.y * 0.40f;
            T2[14] = eye.z + fwd.z * 0.40f;
        }
        glUniform1i(G.u_mode, 0);
        colour(0.35f, 0.36f, 0.40f, 1.0f);
        set_model(T2);
        draw(&G.ctrl[0]);
        /* A coloured ball 90 mm out along each of the model's OWN axes:
         * red = +x, green = +y, blue = +z. Spheres rather than the stretched blocks
         * the first version used — those came out microscopic because the block is
         * already a fixed size and scaling it by a length does not work. A ball at a
         * known offset labels an axis without ambiguity. */
        for (int ax = 0; ax < 3; ax++) {
            mm4_identity(Sm);
            Sm[0] = Sm[5] = Sm[10] = 0.012f;
            Sm[12 + ax] = 0.090f;
            mm4_mul(M2, T2, Sm);
            colour(ax == 0 ? 1.0f : 0.05f, ax == 1 ? 1.0f : 0.05f, ax == 2 ? 1.0f : 0.05f, 1.0f);
            set_model(M2);
            draw(&G.ball);
        }
    }

    /* ---- your hands ---- */
    if (s->hands_valid) {
        glUniform1i(G.u_mode, 0);
        colour(0.16f, 0.17f, 0.19f, 1.0f);
        for (int i = 0; i < 2; i++) {
            float M[16], P[16], K[16];
            mm4_from_pose(P, s->hand[i], 1.0f);
            /* MODEL -> GRIP.
             *
             * Established by rendering the model with its own axes labelled (see
             * CUEVR_CTRLAXES), which is what I should have built before touching the
             * matrix at all rather than after seven attempts at guessing it:
             *
             *   model +y  up the HANDLE, away from the button end
             *   model +z  out of the button face
             *   model +x  across, to the right
             *
             * Grip space runs the handle along Z with -Z toward the thumb, and the
             * thumb is at the button end, so model +y maps to grip +z. That part is
             * settled. The only freedom left is the ROLL about that axis, and it
             * cannot be derived from the model — only looked at. CUEVR_CTRLROLL sets
             * it in degrees so all four can be compared in one go instead of guessed
             * one at a time. */
            const float CTRL_ZMID = 0.0418f;
            /* 180 about the handle. Chosen by comparing all four in one render
             * rather than guessed — the model's axes are known (CUEVR_CTRLAXES) so
             * the roll is the only free parameter, and it is not derivable from the
             * geometry. CUEVR_CTRLROLL still overrides it. */
            float roll = 180.0f;
            { const char *rv = getenv("CUEVR_CTRLROLL"); if (rv) roll = (float)atof(rv); }
            {
                float A[16], R[16];
                mm4_identity(A);
                A[0] = 1.0f;  A[1] = 0.0f;  A[2]  =  0.0f;   /* model x -> grip  x */
                A[4] = 0.0f;  A[5] = 0.0f;  A[6]  =  1.0f;   /* model y -> grip  z (handle) */
                A[8] = 0.0f;  A[9] = -1.0f; A[10] =  0.0f;   /* model z -> grip -y */
                A[13] = CTRL_ZMID;
                float c = cosf(roll * 3.14159265f / 180.0f);
                float sn = sinf(roll * 3.14159265f / 180.0f);
                mm4_identity(R);                             /* about grip Z, the handle */
                R[0] = c;   R[1] = sn;
                R[4] = -sn; R[5] = c;
                mm4_mul(K, R, A);
            }
            mm4_mul(M, P, K);
            set_model(M);
            draw(G.ctrl[i].n ? &G.ctrl[i] : &G.grip);
        }
        /* Where the cue is resting on the bridge: a small pale marker, so the
         * rest adjustment has something to show for itself. */
        if (s->rest_visible) {
            glUniform1i(G.u_mode, 0);
            colour(0.55f, 0.52f, 0.44f, 1.0f);
            float L[16], M2[16];
            mm4_identity(L);
            L[0] = 0.016f; L[5] = 0.016f;
            L[12] = s->rest_pos.x; L[13] = s->rest_pos.y; L[14] = s->rest_pos.z;
            mm4_mul(M2, L, L);
            set_model(L);
            draw(&G.quad);
        }
    }

    /* ---- the HUD panel ---- */
    if (s->hud_visible) {
        glUniform1i(G.u_mode, 2);
        glUniform4f(G.u_colour, 1, 1, 1, 1);
        glBindTexture(GL_TEXTURE_2D, G.hud_tex);
        glDisable(GL_CULL_FACE);
        float P[16];
        MoteVrPose hp;
        hp.p = s->hud_pos;
        hp.q = s->hud_rot;
        mm4_from_pose(P, hp, 1.0f);
        float S[16];
        mm4_identity(S);
        S[0] = s->hud_w;
        S[5] = s->hud_w * (float)CUEVR_HUD_H / (float)CUEVR_HUD_W;
        float M[16];
        mm4_mul(M, P, S);
        set_model(M);
        draw(&G.quad);
        glEnable(GL_CULL_FACE);
    }

    glBindVertexArray(0);
}
