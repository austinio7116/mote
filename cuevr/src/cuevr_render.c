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
#include "cuevr_light.h"
#include "cuevr_glb.h"
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
"uniform vec3  u_lampC[8];\n"   // lamp centres, world space
"uniform vec3  u_lampX[8];\n"   // half-extent vector, one way across the face
"uniform vec3  u_lampZ[8];\n"   // and the other: together they span the plane
"uniform float u_lampG[8];\n"
"uniform vec3  u_lampN[8];\n"   // plane normal, precomputed
"uniform vec2  u_lampI[8];\n"   // 1/|X|^2 and 1/|Z|^2   // how bright each one's reflection is
"uniform int   u_nlamp;\n"
"uniform float u_lampround;\n" // 1 = discs (downlights), 0 = rectangles (shades)
"uniform vec3  u_keyc;\n"      // the light's own colour, multiplying everything
"uniform float u_fill;\n"      // how much of the diffuse arrives from the room
"uniform float u_hudv;\n"
"uniform vec2  u_shadow;\n"
"uniform float u_clothlod;\n"
"uniform float u_rawcol;\n"
"uniform vec3  u_varn;\n"
"uniform highp sampler2DShadow u_shmap;\n"
"uniform mat4  u_shmat;\n"
"uniform float u_shon;\n"
"uniform float u_shtexel;\n"
"uniform float u_shsoft;\n"
"uniform float u_norefl;\n"   // along-grain roughness, across, strength   // debug: show the authored vertex colour, unshaded  // 0 = plain cloth, 1 = the full nap  // penumbra width, umbra darkness      // fraction of the HUD texture's height in use
"in vec3 v_eyepos;\n"
"uniform vec3  u_clothsh;\n"    // cloth bounce tint
"uniform vec3  u_cloth;\n"      // the cloth's own colour
"uniform highp sampler2DArray u_fur;\n"
"uniform highp sampler2D u_nap;\n"
"uniform float u_feltspan;\n"
"uniform vec2  u_half;\n"
"uniform float u_furslice;\n"
"uniform float u_furslices;\n"
"uniform float u_furdbg;\n"
"uniform vec3  u_cshaft;\n"
"uniform vec3  u_csplice;\n"
"uniform vec3  u_cvnr2;\n"
"uniform vec3  u_cwrapc;\n"
"uniform vec3  u_csleevec;\n"
"uniform vec3  u_cringc;\n"
"uniform float u_cpts;\n"
"uniform float u_cptlen;\n"
"uniform float u_cnvnr;\n"
"uniform float u_cwrap;\n"
"uniform float u_csleeve;\n"
"uniform float u_clam;\n"
"uniform float u_cvw;\n"
"uniform float u_cv2on;\n"
"uniform vec3  u_cdiac;\n"
"uniform float u_cdia;\n"
"uniform vec3  u_cvcol[6];\n"
"uniform float u_cnvcol;\n"
"uniform float u_csfig;\n"
"uniform float u_cbfig;\n"
"uniform float u_cishape;\n"
"uniform float u_cipearl;\n"
"uniform float u_cit;\n"
"uniform float u_chand;\n"
"uniform float u_cnarch;\n"
"uniform float u_cpflip;\n"
"uniform float u_cppearl;\n"
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
"float fbm2(vec2 p) { return 0.60*vnoise(p) + 0.28*vnoise(p*2.63) + 0.12*vnoise(p*6.17); }\n"
"vec4 emit(vec3 c, float a, float dither) {\n"
"    // u_keyc is the light's colour, so it multiplies EVERYTHING the light\n"
"    // reaches — a tungsten room does not warm the highlights and leave the\n"
"    // shadows neutral. The HUD sets it to white before it draws, because a\n"
"    // scoreboard is a screen and not a lit surface.\n"
"    vec3 o = u_encode == 1 ? to_srgb(c * u_keyc) : c * u_keyc;\n"
"    return vec4(o + (hash12(gl_FragCoord.xy) - 0.5) * dither / 255.0, a);\n"
"}\n"
"// The rig's diffuse response, in place of a bare N.L.\n"
"//\n"
"// u_fill is how much of a surface's light arrives from the room rather than\n"
"// from the key: nothing at all in a dark hall under a bar of shades, about half\n"
"// in a front room with the ceiling lights on and the walls a metre away. The\n"
"// fill term is hemispherical — brighter facing up, because ceilings and windows\n"
"// are up — which is what lifts the underside of a cushion and the inside of a\n"
"// pocket out of black without flattening the whole table.\n"
"//\n"
"// At u_fill = 0 this is exactly max(dot(N, L), 0.0), so the match rig every\n"
"// existing screenshot was tuned against is untouched.\n"
"// How much of the key reaches this point. 1 = lit, 0 = fully shadowed.\n"
"//\n"
"// One hardware PCF fetch. The bias is along the light rather than a constant,\n"
"// so a surface at a grazing angle to the lamp — the far side of a ball, the\n"
"// slope of a cushion — does not shadow-acne itself, and a ball still meets its\n"
"// own shadow at the contact patch instead of hovering over it.\n"
"float g_sh = 1.0;\n"
"float shadow_at(vec3 wp, vec3 N, vec3 L) {\n"
"    if (u_shon < 0.5) return 1.0;\n"
"    float sl = clamp(1.0 - dot(N, L), 0.0, 1.0);\n"
"    vec4 lp = u_shmat * vec4(wp + N * (0.0015 + 0.006 * sl), 1.0);\n"
"    vec3 q = lp.xyz / lp.w * 0.5 + 0.5;\n"
"    if (q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0 || q.z > 1.0)\n"
"        return 1.0;                 // outside the map: unshadowed, never dark\n"
"    // EIGHT TAPS over a real penumbra. Four taps at one texel is not a soft\n"
"    // shadow, it is a hard test with its staircase filed off — which is what it\n"
"    // looked like. A ball is 52 mm and the shadow under it on cloth, lit by\n"
"    // shades a foot or two wide, has a penumbra of several millimetres, so the\n"
"    // kernel has to be several millimetres too. u_shsoft is that radius in\n"
"    // texels; at 2048 over a 12 ft table a texel is 2.3 mm.\n"
"    float texel = 1.0 / u_shtexel;\n"
"    float r = texel * u_shsoft;\n"
"    const vec2 K[8] = vec2[8](\n"
"        vec2( 0.000,  1.000), vec2( 0.707,  0.707),\n"
"        vec2( 1.000,  0.000), vec2( 0.707, -0.707),\n"
"        vec2( 0.000, -1.000), vec2(-0.707, -0.707),\n"
"        vec2(-1.000,  0.000), vec2(-0.707,  0.707));\n"
"    float sum = 0.0;\n"
"    for (int i = 0; i < 8; i++) {\n"
"        // Two rings, so the middle of the penumbra is sampled too rather than\n"
"        // only its rim — a single ring gives a doughnut, not a gradient.\n"
"        sum += texture(u_shmap, vec3(q.xy + K[i] * r,        q.z));\n"
"        sum += texture(u_shmap, vec3(q.xy + K[i] * r * 0.45, q.z));\n"
"    }\n"
"    return sum * 0.0625;\n"
"}\n"
"float diffuse(vec3 N, vec3 L) {\n"
"    float ndl = max(dot(N, L), 0.0) * g_sh;\n"
"    return mix(ndl, 0.45 + 0.55 * (0.5 + 0.5 * N.y), u_fill);\n"
"}\n"
"// The same, for the surfaces whose normals are two-sided — the cloth over a\n"
"// cushion nose, the fur shells — where the shaded half should go dim rather\n"
"// than go out.\n"
"float diffuse_abs(vec3 N, vec3 L) {\n"
"    return mix(abs(dot(N, L)) * g_sh, 0.45 + 0.55 * (0.5 + 0.5 * abs(N.y)), u_fill);\n"
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
"// Knots.\n"
"//\n"
"// A knot is the CUT END OF A BRANCH, and that is three separate things, not one:\n"
"//\n"
"//   1. its own growth rings, concentric on the branch centre and much tighter than\n"
"//      the trunk's, because a branch is thin and grew slowly;\n"
"//   2. a hard dark rim where branch met trunk, often almost black;\n"
"//   3. the trunk's grain SWEEPING AROUND it — the rings pile up on the approach\n"
"//      side and stream past, because the branch was in the way as the trunk grew.\n"
"//\n"
"// The first version had only a filled dark ellipse and a smooth bulge, which is why\n"
"// it read as a stain rather than a knot. The sweep is the part the eye actually\n"
"// uses to identify one.\n"
"//\n"
"// One candidate per cell along the board; position, size and presence from a hash so\n"
"// no two rails match. No texture fetches, all arithmetic.\n"
"struct Knot { float bulge; float core; float ring; float rim; };\n"
"\n"
"Knot knots(vec2 wq) {\n"
"    // Sparse. Good rail stock is largely clear timber — a knot every few\n"
"    // hundred millimetres is a feature, one every 145 mm is a fence post.\n"
"    const float CELL  = 0.38;             /* cells along the rail */\n"
"    const float CELLY = 0.075;            /* and across it */\n"
"    Knot k;\n"
"    k.bulge = 0.0; k.core = 0.0; k.ring = 0.0; k.rim = 0.0;\n"
"    // Cells in BOTH axes. Placing knot centres within a few centimetres of\n"
"    // wq.y == 0 was the reason none ever appeared: on a side rail wq.y is the\n"
"    // world Z, around half a metre, so every knot sat half a metre off the board\n"
"    // and the distance test could never pass. Knots have to be placed in the\n"
"    // board's own coordinates, which means celling the across axis as well.\n"
"    float ci = floor(wq.x / CELL);\n"
"    float cj = floor(wq.y / CELLY);\n"
"    for (int i = -1; i <= 1; i++) {\n"
"        float cx = ci + float(i);\n"
"        if (hash12(vec2(cx, cj + 3.7)) < 0.72) continue;   /* most cells clear */\n"
"        vec2 kp = vec2((cx + 0.18 + 0.64 * hash12(vec2(cx, cj + 11.0))) * CELL,\n"
"                       (cj + 0.20 + 0.60 * hash12(vec2(cx, cj + 29.0))) * CELLY);\n"
"        float kr = 0.0075 + 0.0125 * hash12(vec2(cx, cj + 17.0));\n"
"        /* Knots are elliptical because the branch is cut at an angle. */\n"
"        // Nearly round. At 0.62 they came out as long ovals — a branch cut close\n"
"        // to square through is only slightly elliptical, and the rail is seen at a\n"
"        // grazing angle on top of that, which stretches them again.\n"
"        vec2 dv = (wq - kp) / vec2(1.0, 0.88);\n"
"        float r = length(dv);\n"
"\n"
"        /* 3: the trunk grain streams past. Strongest just outside the rim and\n"
"         * decaying over a couple of knot radii — a smooth Gaussian on its own just\n"
"         * translates the rings, so this is biased along the board to make them pile\n"
"         * up on one side and stretch on the other. */\n"
"         float g = exp(-(r * r) / (kr * kr * 3.0));\n"
"         k.bulge += g * kr * 4.5 * (1.0 + 0.9 * sign(dv.x));\n"
"\n"
"        if (r < kr * 1.35) {\n"
"            /* 1: the branch's own rings, tight and concentric. */\n"
"            k.ring = max(k.ring, fract(r / (kr * 0.16)));\n"
"            k.core = max(k.core, 1.0 - smoothstep(kr * 0.80, kr * 1.30, r));\n"
"            /* 2: the rim, a narrow hard band at the boundary. */\n"
"            k.rim = max(k.rim, (1.0 - smoothstep(0.0, kr * 0.22, abs(r - kr)))); \n"
"        }\n"
"    }\n"
"    return k;\n"
"}\n"
"\n"
"vec3 timber(vec3 base, vec2 wq, vec2 warp, vec2 fine, vec3 nrm, vec3 tang, vec3 Ldir,\n"
"            float rings_per_m,\n"
"            out float varnish) {\n"
"    const float PITH_OFF   = 0.085;   /* metres from the board to the log centre */\n"
"    // Rings per metre, from the caller. 105 is ~9.5 mm a season, right for a\n"
"    // hardwood rail — and quite wrong for a 30 mm cue butt, where it puts about\n"
"    // nine broad bands round the whole circumference. Same wood, different\n"
"    // scale: everything else in here, the cathedral figure, the pores, the ray\n"
"    // fleck, the varnish, is expressed in RINGs and so follows automatically.\n"
"    float RINGS_PER_M = rings_per_m;\n"
"    float RING = 1.0 / RINGS_PER_M;   /* one ring, in metres */\n"
"    // EVERY perturbation below is a fraction of RING. Expressed in absolute\n"
"    // metres they were enormous next to it — the first attempt displaced the pith\n"
"    // distance by up to 30 mm on a 4.8 mm ring, which is six rings of shift per\n"
"    // pixel, so the rings scrambled into speckle and it came out looking like cork.\n"
"\n"
"    // 1 + 2: warped distance from the pith line.\n"
"    float across = wq.y + PITH_OFF;\n"
"    float along  = wq.x;\n"
"    // CATHEDRAL FIGURE. A flat-sawn board cuts the rings at a shallow angle, so\n"
"    // they come out as nested arches rather than parallel bands — the pattern\n"
"    // everyone recognises as wood. It falls out of letting the effective depth\n"
"    // through the log vary slowly ALONG the board: where the cut is closer to the\n"
"    // pith the rings pinch together into an arch, and where it is further they\n"
"    // spread. Parallel bands are quarter-sawn stock, which a rail rarely is.\n"
"    // The arch amplitude has to be several RINGS to read as cathedral figure, not\n"
"    // a fraction of one. At 13 mm on a 9.5 mm ring it was a gentle wobble.\n"
"    float depth = 0.030 + RING * 5.5 * sin(along * 5.0 + warp.x * 1.1)\n"
"                        + RING * 2.2 * sin(along * 13.0 + warp.y * 0.7);\n"
"    Knot kn = knots(wq);\n"
"    if (u_rawcol > 1.5) { kn.bulge = 0.0; kn.core = 0.0; kn.ring = 0.0; kn.rim = 0.0; }\n"
"    float d = length(vec2(across + warp.x * RING * 1.6 + kn.bulge, depth));\n"
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
"    // The knot. Same TIMBER, not a feature drawn on top of it.\n"
"    //\n"
"    // A knot is the branch the trunk grew round: identical wood, cut across so\n"
"    // its rings are tight and closed instead of long and open. So it belongs in\n"
"    // the same colour range as the rings around it — the latewood bands land at\n"
"    // about 0.81 of the base, and the knot has no business being darker or\n"
"    // browner than they are. It was at 0.52 of base with the red pushed 16 per\n"
"    // cent and the blue pulled 22, ringed in near-black: a loud, hot blob that\n"
"    // read as a bolt head rather than as figure.\n"
"    //\n"
"    // No hue shift at all now, and the swing across the knot's own rings is\n"
"    // wider than its offset from the base — which is what makes it read as\n"
"    // PATTERN. You see it as a tight whorl in the grain, and only when you look.\n"
"    vec3 kbody = base * (0.74 + 0.20 * kn.ring);\n"
"    c = mix(c, kbody, kn.core * 0.85);\n"
"    // The rim, where the branch met the trunk. Present, because the grain does\n"
"    // close hard there — but at the darkness of a latewood band, not of a hole.\n"
"    c = mix(c, base * 0.62, kn.rim * 0.45);\n"
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
"        // THE GRAIN DIRECTION, passed in — not one derived from the normal here.\n"
"        //\n"
"        // It used to pick a reference axis with `abs(n.y) < 0.9 ? up : right`\n"
"        // and cross it with the normal. That branch is a cliff: a surface whose\n"
"        // normal drifts across 0.9 gets a completely different tangent, and the\n"
"        // anisotropic lobe swings with it. On the rail top around a pocket the\n"
"        // crease-smoothed normals tilt just enough to cross it, so the varnish\n"
"        // lit up in a bright patch — square, because the bore is notched out of\n"
"        // the plank as a bounding BOX and the triangles bordering it form a\n"
"        // square ring around the round hole.\n"
"        //\n"
"        // The caller already knows which way the grain runs; it is the same\n"
"        // axis wq is measured along. Using it is both continuous and correct —\n"
"        // the sheen on varnished timber stretches ALONG the fibres.\n"
"        vec3 n = normalize(nrm);\n"
"        vec3 tg = normalize(tang - n * dot(tang, n) + vec3(1e-6));\n"
"        vec3 Vv = normalize(v_eyepos - v_world);\n"
"        // A WARD-STYLE ANISOTROPIC LOBE ON THE HALF-VECTOR.\n"
"        //\n"
"        // This was Kajiya-Kay: pow(sin.sin - cos.cos, 34) over the tangent, the\n"
"        // light and the view. That is a model for FIBRES, where there is no\n"
"        // normal around the strand, AND IT NEVER REFERENCES THE SURFACE NORMAL.\n"
"        // On a flat plank it therefore has no idea which way the wood faces: on\n"
"        // a horizontal rail top under an overhead lamp both L and V sit nearly\n"
"        // perpendicular to the grain, the term saturates to 1 across the whole\n"
"        // surface, and a highlight with no shape reads as no highlight at all.\n"
"        // The verticals looked varnished and the tops looked flat — and because\n"
"        // the only remaining variation came from N.L, ANY perturbation of the\n"
"        // normal showed up as a hard edge. That is what drew a square around\n"
"        // every pocket, where the crease-smoothed normals tilt at the bore.\n"
"        //\n"
"        // A varnished plank is a rough MIRROR, so the lobe belongs on the half\n"
"        // vector and must fall off away from the normal. Stretching it along the\n"
"        // grain and pinching it across gives the sheen that slides down a rail\n"
"        // as you move, on every face, at any orientation.\n"
"        // The Kajiya-Kay fibre lobe, which is the right shape for varnished\n"
"        // timber — the sheen stretches along the fibres and slides as you move.\n"
"        //\n"
"        // THE EXPONENT WAS THE BUG. At 34 the lobe only fires when the grain\n"
"        // runs nearly across your line of sight. Worked through for a rail top\n"
"        // with the eye at 55 degrees and the key overhead: a SIDE rail, grain\n"
"        // along x, gives T.L = 0.10, T.V = 0 and a lobe of 0.995^34 = 0.84 —\n"
"        // full sheen. The END rail, grain along z, gives T.L = 0.20, T.V = 0.57\n"
"        // and 0.69^34 = 0.000003 — nothing at all. At a corner the two meet, so\n"
"        // one side of the mitre was varnished and the other was dead, and the\n"
"        // only other lit patch was the ring at the bore where the smoothed\n"
"        // normals push the tangent back onto the lobe. Those lit patches were\n"
"        // never the artefact; the dead wood around them was.\n"
"        //\n"
"        // A finish is rough, so the lobe is broad. u_varn.x is the exponent.\n"
"        vec3 tgv = tg;\n"
"        float TdL = dot(tgv, Ldir), TdV = dot(tgv, Vv);\n"
"        float sL = sqrt(max(1.0 - TdL * TdL, 0.0));\n"
"        float sV = sqrt(max(1.0 - TdV * TdV, 0.0));\n"
"        float aniso = pow(max(sL * sV - TdL * TdV, 0.0), u_varn.x);\n"
"        // Weighted by N.L, because a specular still needs the light to REACH the\n"
"        // surface. Without it the anisotropic lobe blew the inside of the pocket\n"
"        // throat out to white — a surface facing away from the lamp was returning\n"
"        // a full highlight. And the amplitude was far too high for a rail.\n"
"        // ABSOLUTE. The table mesh is DOUBLE-SIDED — cue_render authors it for a\n"
"        // software rasteriser that does not cull, so a face normal points\n"
"        // whichever way the winding happened to fall. That is why the diffuse\n"
"        // uses diffuse_abs(). The varnish did not, and a rail-top quad wound so\n"
"        // its normal points DOWN gives dot(n, L) = -0.975, so lit = 0 and the\n"
"        // sheen was switched off across the entire top of the frame, all four\n"
"        // sides. The only place it survived was the ring at each pocket bore,\n"
"        // where the crease-smoothed normals average with the vertical bore wall\n"
"        // and tilt far enough off straight-down for the dot to come back\n"
"        // positive. That ring was never an artefact — it was the only correctly\n"
"        // lit wood on the rail, which is exactly what it looked like.\n"
"        float lit = abs(dot(n, Ldir));\n"
"        // SCALED BY THE TIMBER'S OWN BRIGHTNESS.\n"
"        //\n"
"        // The varnish is added in linear space after the diffuse, which is\n"
"        // right, but its size was tuned against mid-brown rail stock. Put the\n"
"        // same absolute highlight on a surface a third as bright — the dark\n"
"        // bore wall inside a pocket, shade565(woodt, 0.42) — and it does not\n"
"        // sheen it, it swamps it: measured, an authored (49,28,8) came out at\n"
"        // (144,100,75), three times brighter and washed grey. That is the pale\n"
"        // square that has been sitting beside every pocket on every table.\n"
"        //\n"
"        // A finish takes the colour of what it is on. Scaling by the base's own\n"
"        // luminance keeps the rails exactly as they were and lets dark timber\n"
"        // stay dark, which is also simply what varnish does.\n"
"        float base_l = dot(base, vec3(0.30, 0.59, 0.11));\n"
"        float shine  = clamp(base_l / 0.34, 0.0, 1.0);\n"
"        // THE ANISOTROPIC LOBE ONLY.\n"
"        //\n"
"        // There used to be a broad Blinn term alongside it, on the raw normal.\n"
"        // Bisected against the pale patch that sat on the rail top around every\n"
"        // pocket: with only the anisotropic lobe it is gone; with only the broad\n"
"        // one it is plainly there. A generic highlight taken off the normal is\n"
"        // exactly what misbehaves where the crease-smoothed normals tilt at the\n"
"        // bore, and the bore is notched out of the plank as a bounding BOX, so\n"
"        // the triangles bordering it form a square ring round a round hole.\n"
"        //\n"
"        // Losing it costs nothing. The anisotropic lobe is the one that makes\n"
"        // timber look VARNISHED — a sheen stretched along the fibres, sliding\n"
"        // as you move — and the broad term was a general gloss sitting under it.\n"
"        varnish = clamp(aniso, 0.0, 3.0) * (0.30 + 0.70 * late) * u_varn.z * lit * shine;\n"
"    }\n"
"    return c;\n"
"}\n"
"\n"
"float mark_cov(vec2 q, float aa) {\n"
"    // The feather STRADDLES the edge rather than being added outside it.\n"
"    // smoothstep(w, w + aa, d) puts the whole transition beyond the line, so\n"
"    // the mark grew by however big fwidth was — and fwidth grows with distance\n"
"    // and with grazing angle, which is why the far side of the table carried a\n"
"    // fat blurred baulk line and the near side a crisp one. Centred, the width\n"
"    // is the width wherever you stand.\n"
"    // A mark thinner than a pixel cannot be drawn at its own width without\n"
"    // breaking into dashes, so it is widened to one pixel and DIMMED by\n"
"    // however much it was widened. That keeps the ink constant: the line reads\n"
"    // the same weight across the whole table and simply fades away into the\n"
"    // distance instead of flickering.\n"
"    float px = min(aa, 0.010);\n"
"    float h  = px * 0.5;\n"
"    float m = 0.0;\n"
"    if (u_linew > 0.0) {\n"
"        float w = max(u_linew, h);\n"
"        float fade = u_linew / w;\n"
"        float dl = abs(q.x - u_baulk);\n"
"        m = (1.0 - smoothstep(w - h, w + h, dl)) * fade;\n"
"        if (u_drad > 0.0 && q.x < u_baulk) {\n"
"            float dr = abs(length(q - vec2(u_baulk, 0.0)) - u_drad);\n"
"            m = max(m, (1.0 - smoothstep(w - h, w + h, dr)) * fade);\n"
"        }\n"
"    }\n"
"    for (int i = 0; i < 8; i++) {\n"
"        if (i >= u_nspot) break;\n"
"        float sr = max(u_spotr, h);\n"
"        float sf = u_spotr / sr;\n"
"        float d = length(q - u_spots[i]);\n"
"        m = max(m, (1.0 - smoothstep(sr - h, sr + h, d)) * sf);\n"
"    }\n"
"    float g1 = vnoise(q * 1600.0);\n"
"    float g2 = vnoise(q * 420.0 + 3.1);\n"
"    return clamp(m * (0.58 + 0.44 * g1 + 0.22 * g2), 0.0, 1.0);\n"
"}\n"
"void main() {\n"

"    vec3 L = normalize(u_light);\n"
"    g_sh = shadow_at(v_world, normalize(v_nrm), L);\n"
"    if (u_mode == 2) {\n"
"        // Only the top u_hudv of the panel texture is in use — see\n"
"        // CUEVR_HUD_LH. v runs from the top, so this is a straight scale.\n"
"        vec4 t = texture(u_tex, vec2(v_uv.x, v_uv.y * u_hudv));\n"
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
"        float diff = diffuse(nw, L);\n"
"        float down = max(-nw.y, 0.0);\n"
"        // Diffuse, then the cloth's bounce ADDED rather than mixed in.\n"
"        // The handheld lerps up to 82% toward the cloth tint on the\n"
"        // shadow side, which at 128x128 reads as 'in shadow' and at this\n"
"        // size reads as a red ball turning muddy green. Adding the bounce\n"
"        // instead lights the underside without draining the hue out of it,\n"
"        // and a snooker ball under a lamp is a *saturated* object.\n"
"        // CONTACT OCCLUSION. The bounce term was weighted by `down`, so the\n"
"        // closer a piece of ball faced the cloth the MORE light it was given —\n"
"        // and nothing at all represented the ball shadowing itself against the\n"
"        // table it is resting on. The underside glowed. A ball sitting on cloth\n"
"        // has its contact patch in near darkness and it lifts over the bottom\n"
"        // third; the bounce is real, but it is what fills that shadow, not\n"
"        // something added on top of a surface that should already be dark.\n"
"        float below = clamp(down, 0.0, 1.0);\n"
"        float occ   = 1.0 - 0.72 * below * below;\n"
"        vec3 c = bc * (0.52 + 0.62 * diff) * occ\n"
"               + u_clothsh * (below * 0.16 + 0.08) * occ;\n"
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
"        {\n"
"            // Intersect the reflected ray with each fixture's own PLANE, not\n"
"            // with a horizontal one. A shade hangs face down and a window\n"
"            // stands on its edge, and the first version, which solved for the\n"
"            // height of the lamp, simply could not see a vertical source at\n"
"            // all — the window mode reflected nothing.\n"
"            for (int i = 0; i < u_nlamp; i++) {\n"
"                if (u_norefl > 0.5) break;   // measurement only\n"
"                // The plane normal and the two inverse-square lengths are the\n"
"                // SAME for every fragment of every ball — they change when a\n"
"                // lamp moves, which is once a frame. They were being recomputed\n"
"                // per fragment per lamp: a cross product and two dot products,\n"
"                // eight times over, on every pixel of every ball. Measured at\n"
"                // 0.87 ms of a 7.31 ms frame, which is 12% for arithmetic whose\n"
"                // answer never changes. Precomputed on the CPU now; the picture\n"
"                // is identical.\n"
"                //\n"
"                // Distance-gating the loop was the other candidate and would\n"
"                // not have worked: the cost is per FRAGMENT, so it is dominated\n"
"                // by near balls covering many pixels, and those are exactly the\n"
"                // ones that cannot be skipped.\n"
"                vec3 pn = u_lampN[i];\n"
"                float dn = dot(Rv, pn);\n"
"                if (abs(dn) < 1e-6) continue;\n"
"                float t = dot(u_lampC[i] - v_world, pn) / dn;\n"
"                if (t <= 0.0) continue;\n"
"                vec3 d = (v_world + Rv * t) - u_lampC[i];\n"
"                float a = dot(d, u_lampX[i]) * u_lampI[i].x;\n"
"                float b = dot(d, u_lampZ[i]) * u_lampI[i].y;\n"
"                // A shade has a hard edge and a hot centre. smoothstep over\n"
"                // the last few percent keeps it from aliasing to a crawling\n"
"                // staircase as the ball rolls. A downlight is a disc, and a\n"
"                // square highlight is the giveaway that it was never modelled.\n"
"                float e = mix(max(abs(a), abs(b)), length(vec2(a, b)),\n"
"                              u_lampround);\n"
"                refl += (1.0 - smoothstep(0.88, 1.0, e)) * (1.0 - 0.25 * e)\n"
"                      * u_lampG[i];\n"
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
"        // The turned profile, mirroring cue_radius() on the CPU: one linear\n"
"        // taper from the ferrule to the butt. The veneer geometry needs it.\n"
"        float cue_r = 0.0051 + clamp((t*1.45 - 0.032)/1.378, 0.0, 1.0) * 0.0109;\n"
"        float butt_varn = 0.0;\n"
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
"            // ASH ARROWS. Growth rings are cones down the tree; a cylinder\n"
"            // turned from it cuts them as nested chevrons pointing at the\n"
"            // tip on the two flat-sawn faces, easing into long straight\n"
"            // lines round the quarter faces — cos(ang) is exactly that\n"
"            // phase. Bold, near-black on cream, per the reference; broken\n"
"            // up along their run because real grain is.\n"
"            float ang = a * 6.2831853;\n"
"            /* IRREGULAR, like the tree was. Perfectly periodic rings read\n"
"             * as machining: real ash varies its ring spacing, and every ring\n"
"             * has its own weight — some bold, some faint, some almost gone.\n"
"             * The spacing jitters through a warped t, and each ring hashes\n"
"             * its own darkness and width off its index. */\n"
"            /* THE GRAIN RUNS DOWN THE SHAFT — a cue is always cut that way.\n"
"             * Where the stock widens, the ring cones surface as long curved\n"
"             * cathedral loops POINTING FORWARD along the top and bottom\n"
"             * faces, while the sides show parallel lines. So the phase\n"
"             * varies strongly AROUND the cue and only slowly along it — the\n"
"             * previous balance was inverted and drew transverse rings. */\n"
"            float tw   = t + 0.010 * (fbm2(vec2(t * 14.0, cos(ang) * 0.7)) - 0.5);\n"
"            float ph   = tw * 16.0 + (7.0 * cue_r / 0.016) * cos(ang)\n"
"                       + 1.1 * fbm2(vec2(t * 5.0, cos(ang) * 2.2));\n"
"            float ring = fract(ph);\n"
"            float rid  = floor(ph);\n"
"            float rs   = 0.35 + 0.65 * hash12(vec2(rid, 3.7));   /* ring strength */\n"
"            float dl   = min(ring, 1.0 - ring);\n"
"            float lw   = (0.045 + 0.06 * hash12(vec2(rid, 9.1)))\n"
"                       + 0.04 * fbm2(vec2(ang * 2.0, t * 40.0));\n"
"            float line = 1.0 - smoothstep(lw * 0.4, lw, dl);\n"
"            /* Lines PERSIST and vary rather than fragmenting: real ash grain\n"
"             * runs in long continuous chevrons, lighter and darker along its\n"
"             * run, and the old mask was snapping it into blobs. */\n"
"            float mask = 0.45 + 0.55 * smoothstep(0.30, 0.70, fbm2(vec2(t * 6.0, ang * 2.0)));\n"
"            vec3  dark = ash * vec3(0.36, 0.34, 0.32);\n"
"            c = ash * (0.92 + 0.12 * fbm2(vec2(t * 6.0, ang * 0.6)));\n"
"            /* Grain strength follows the stock: bold chevrons where the cue\n"
"             * is wide, fading to fine near-straight lines toward the tip —\n"
"             * full-contrast arcs on the thin half read as rings. */\n"
"            float gs = clamp((cue_r - 0.0051) / 0.0109, 0.0, 1.0);\n"
"            c = mix(c, dark, line * mask * rs * (0.35 + 0.75 * gs));\n"
"            if (u_csfig > 0.5) {\n"
"                // MAPLE: pale, close-grained, nearly plain — a faint curl\n"
"                // across the shaft is all it shows.\n"
"                float curl = 0.5 + 0.5 * sin(t * 160.0 + 3.0 * fbm2(vec2(t * 20.0, ang * 0.3)));\n"
"                c = u_cshaft * (0.95 + 0.07 * curl);\n"
"            }\n"
"            // ---- the butt of a three-quarter jointed cue --------------- *\n"
"            //\n"
"            // Read off the reference rather than invented. From the joint down:\n"
"            //\n"
"            //   ash shaft, into which four EBONY points rise and taper to a\n"
"            //     needle — this is the main splice and it is at the TOP;\n"
"            //   a long plain black section, and the BRASS COLLAR lives here,\n"
"            //     nowhere near the points;\n"
"            //   four long slender BURR points rising into the black from below,\n"
"            //     each one outlined in a fine coloured veneer;\n"
"            //   a burr butt, an ivory badge on the very end, a black cap band.\n"
"            //\n"
"            // The old version had one splice, points half this length, solid\n"
"            // ebony where the burr should be, a veneer flashed near the edges\n"
"            // instead of tracing the outline, and the collar cutting straight\n"
"            // through the points.\n"
"            //\n"
"            // Points are widest at their base and taper toward the tip of the\n"
"            // cue, and pow() below 1 is what makes them SLENDER — a linear\n"
"            // taper gives fat triangles, and a real splice is nearly a needle\n"
"            // for the last third of its length.\n"
"            // How many points, and how far they run. A Peradon Classic has no\n"
"            // splice at all, a Crown is short and a Royal is long; that is the\n"
"            // difference between them, not the colour.\n"
"            float f = fract(a * max(u_cpts, 1.0));\n"
"            float d = min(f, 1.0 - f);          /* distance to a point centre */\n"
"            float PL = u_cptlen;\n"
"\n"
"            /* 1. the main splice: ebony rising into the ash */\n"
"            /* An American cue packs its whole composition BUTT-SIDE of the\n"
"             * brass joint: forearm points, then wrap, then sleeve, tight. The\n"
"             * British 3/4 layout keeps the splice up the shaft. */\n"
"            float ms_base = (u_cwrap > 0.5) ? 0.790 : 0.610;\n"
"            float ms_tip  = ms_base - ((u_cwrap > 0.5) ? 0.115 : 0.140) * PL;\n"
"            if (t > ms_tip && u_cpts > 0.5) {\n"
"                // HAND-SPLICED points are ROUNDED at the top and sit at\n"
"                // slightly UNEVEN heights — four splices laid over the ash by\n"
"                // hand, no two alike. MACHINE-spliced points are dead sharp\n"
"                // and identical. That distinction, not the colour, is what\n"
"                // says which kind of cue you are holding.\n"
"                float pid  = floor(a * max(u_cpts, 1.0));\n"
"                float unev = (u_chand > 0.5) ? (hash12(vec2(pid, 7.3)) - 0.5) * 0.016 : 0.0;\n"
"                float k = clamp((t - (ms_tip + unev)) / (ms_base - ms_tip), 0.0, 1.0);\n"
"                float kr = (u_chand > 0.5) ? 0.045 : 0.004;\n"
"                /* Adjacent splices MEET: hw runs past 0.5 so neighbouring\n"
"                 * arches touch at the quarter boundaries and the veneers\n"
"                 * pinch closed into scallops BEFORE the plain black starts —\n"
"                 * a hard cut at ms_base truncated every arch mid-curve. */\n"
"                /* A prong tapers nearly STRAIGHT and rounds only at the very\n"
"                 * end — a rounded exponent made hw shoot wide at the tip and\n"
"                 * every point ended in a blunt chop. The hyperbola runs\n"
"                 * asymptotically linear with a soft tip of radius kr; machine\n"
"                 * points keep the razor V (kr ~ 0). Still reaches 0.60 so\n"
"                 * neighbouring arches meet and scallop before the black. */\n"
"                float hw = 0.60 * (sqrt(k * k + kr * kr) - kr) / (sqrt(1.0 + kr * kr) - kr);\n"
"                /* A glue line is CRISP. Softness proportional to hw blurred\n"
"                 * the edge by millimetres; the edge is one pixel wide at any\n"
"                 * distance, hand or machine — the two differ in SHAPE, not\n"
"                 * in focus. */\n"
"                float aa = fwidth(d) * 1.2;\n"
"                /* Nothing narrower than a pixel draws: where the prong has\n"
"                 * just begun, hw < aa and the AA band alone painted a faint\n"
"                 * black wash across |d| < aa — at grazing angles fwidth(d)\n"
"                 * spans the whole visible shaft, which put a thin dark band\n"
"                 * right across the cue at exactly prong-start. */\n"
"                float e = (hw > aa) ? smoothstep(hw + aa, hw - aa, d) : 0.0;\n"
"                c = mix(c, ebony, e);\n"
"                if (u_cflash > 0.5 && k > 0.003) {\n"
"                    /* The veneer is glued to the SPLICE — where there is no\n"
"                     * panel yet there is no veneer. Without this gate hw is\n"
"                     * zero above the apex and abs(d - hw) collapses to d,\n"
"                     * which drew a colour spike up the centreline past the\n"
"                     * curve and streaks up the ash before the splice. */\n"
"                    // Same cut-sheet geometry as the butt splice below: the\n"
"                    // veneer is a slip of constant thickness in the joint, so\n"
"                    // its width on the surface goes as 1/r and broadens where\n"
"                    // the joint runs oblique. This was a smoothstep from zero,\n"
"                    // full strength only exactly on the edge, so up the shaft\n"
"                    // it read as a row of blue dots rather than an inlay.\n"
"                    float edge = abs(d - hw);\n"
"                    float arc  = cue_r * 1.5707963;\n"
"                    float dhwx = 0.42 * 0.62 * pow(max(k, 0.02), -0.38)\n"
"                               / ((ms_base - ms_tip) * 1.45);\n"
"                    float gmag = sqrt(1.0/(arc*arc) + dhwx*dhwx);\n"
"                    float wln  = 0.5 * u_cvw * gmag;\n"
"                    float px   = fwidth(d);\n"
"                    float wdrw = max(wln, px);\n"
"                    float fade = wln / wdrw;\n"
"                    float ln   = 1.0 - smoothstep(wdrw, wdrw + px, edge);\n"
"                    ln *= step(px * 1.3, hw);   /* no veneer before the arch is a pixel wide */\n"
"                    // THE STACK. A hand splice lays several slips in the one\n"
"                    // joint and a Taylor Made laminate lays eight or ten, which\n"
"                    // is why they read as nested Vs of colour rather than as an\n"
"                    // outline. Each sits one thickness outside the last and\n"
"                    // they alternate, pale against coloured, as they are cut.\n"
"                    // A stack of four or more is a LAMINATE, and laminate\n"
"                    // bands are cut through figured wood — their edges wave.\n"
"                    float wob = (u_cnvnr >= 4.0 && u_cnvcol < 0.5)\n"
"                              ? (fbm2(vec2(a * 9.0, t * 55.0)) - 0.5) * 0.55 : 0.0;\n"
"                    float stackon = 1.0;\n"
"                    for (int vi = 1; vi < 8; vi++) {\n"
"                        if (float(vi) >= u_cnvnr) break;\n"
"                        float ei = abs(edge - wln * 2.0 * float(vi) * (1.0 + wob));\n"
"                        float li = 1.0 - smoothstep(wdrw, wdrw + px, ei);\n"
"                        int  ci = (vi - 1) - ((vi - 1) / 6) * 6;\n"
"                        vec3 vc = (u_cnvcol > 0.5) ? u_cvcol[ci]\n"
"                                : ((mod(float(vi), 2.0) < 0.5) ? u_caccent : u_cvnr2);\n"
"                        c = mix(c, vc, li * 0.92 * fade * stackon);\n"
"                    }\n"
"                    c = mix(c, u_caccent, ln * 0.95 * fade);\n"
"                }\n"
"            }\n"
"            if (t > ms_base) {\n"
"                c = ebony;\n"
"                /* The laminate lives in the BUTT: standard dark splices at\n"
"                 * the shaft, plain black through the brass line, then the\n"
"                 * festoon blooms out of the black to the cap. */\n"
"                if (u_cnvcol > 0.5 && t > 0.72) {\n"
"                    float lay = cue_r * cos((a - 0.125) * 6.2831853) * 360.0\n"
"                              + t * 55.0 + 2.5 * fbm2(vec2(t * 7.0, a * 3.0));\n"
"                    float m2 = fract(lay * 0.5) * 2.0;\n"
"                    float fe = 0.10 + 0.45 * fbm2(vec2(t * 220.0, a * 45.0));\n"
"                    float bm = smoothstep(0.5 - fe, 0.5 + fe, abs(m2 - 1.0));\n"
"                    /* fade the banding out as it drops under a pixel, or the\n"
"                     * flank stripes alias into zebra hash */\n"
"                    float lf = clamp(1.2 / max(fwidth(lay), 1e-3) - 0.15, 0.0, 1.0);\n"
"                    bm = mix(0.55, bm, lf);\n"
"                    bm *= smoothstep(0.72, 0.84, t);\n"
"                    int  li2 = int(mod(floor(lay * 0.5), max(u_cnvcol, 1.0)));\n"
"                    vec3 lamc = u_cvcol[li2] * (0.80 + 0.35 * fbm2(vec2(t * 90.0, a * 16.0)));\n"
"                    c = mix(vec3(0.05, 0.045, 0.045), lamc, bm);\n"
"                }\n"
"            }\n"
"\n"
"            /* 2. the burr splice: figured wood rising into the black. Long and\n"
"             *    slender — it runs a third of the whole cue. */\n"
"            /* The butt panel is a SHORT, WIDE arch — the reference domes span\n"
"             * maybe 15 cm and most of the face. At 30 cm long even a rounded\n"
"             * apex reads as a spike; the curve only shows at the right aspect. */\n"
"            float bs_base = 0.945, bs_tip = 0.945 - (0.085 + 0.065 * PL);\n"
"            if (t > bs_tip && u_cpts > 0.5 && u_cnvcol < 0.5 && u_cnarch > 0.0) {\n"
"                /* THE BUTT PANEL, as the real thing is cut (single-sided: one\n"
"                 * set of inlays on the top face, badge flat at the back).\n"
"                 *\n"
"                 * TWO nested curves with DIFFERENT noses, not one curve with\n"
"                 * an outline. The pale facing is its own curve — blunter,\n"
"                 * starting higher — and the dark panel begins lower with a\n"
"                 * longer nose inside it. The band between them is therefore a\n"
"                 * THICK CRESCENT over the apex, tapering to thin lines down\n"
"                 * the sides, which is exactly how the reference reads and\n"
"                 * exactly what a constant-width outline can never do. */\n"
"                float db  = abs(fract(a + 0.375) - 0.5) * 4.0;\n"
"                float tt2 = (u_cpflip > 0.5) ? (bs_tip + bs_base - t) : t;\n"
"                float L   = bs_base - bs_tip;\n"
"                float aa2 = fwidth(db) * 1.2;\n"
"                /* the panel timber, modestly figured across the grain */\n"
"                vec3 burr = u_cburr * (0.86 + 0.28 * fbm2(vec2(t * 55.0, a * 9.0)));\n"
"                vec3 pane = burr;\n"
"                if (u_cppearl > 0.5) {\n"
"                    float mp = 0.5 * fbm2(vec2(t * 42.0, a * 7.0))\n"
"                             + 0.5 * fbm2(vec2(t * 170.0, a * 30.0));\n"
"                    pane = mix(u_cdiac * 0.50, vec3(1.0), smoothstep(0.28, 0.82, mp));\n"
"                    gloss = 120.0; spec_k = 0.55; butt_varn *= 0.4;\n"
"                }\n"
"                if (u_cpflip < 0.5)\n"
"                for (int arch = 0; arch < 2; arch++) {\n"
"                    if (arch == 1 && u_cnarch < 1.5) break;\n"
"                    float off = float(arch) * 0.34 * L;\n"
"                    /* the pale facing: blunt nose, starts first */\n"
"                    float ko  = clamp((tt2 - bs_tip - off) / L, 0.0, 1.0);\n"
"                    float keo = clamp(ko / 0.24, 0.0, 1.0);\n"
"                    float hwo = 0.42 * sqrt(1.0 - (1.0 - keo) * (1.0 - keo));\n"
"                    /* the dark panel: longer, softer nose, inside it */\n"
"                    float ki  = clamp((tt2 - bs_tip - off - 0.055 * L) / L, 0.0, 1.0);\n"
"                    float kei = clamp(ki / 0.55, 0.0, 1.0);\n"
"                    float hwi = 0.360 * sqrt(1.0 - (1.0 - kei) * (1.0 - kei));\n"
"                    /* The curves never meet. The angular gap is constant but\n"
"                     * the cap radius shrinks, so in millimetres the band was\n"
"                     * pinching shut at the rounded end — the panel pulls in a\n"
"                     * touch over the cap so the facing keeps a visible margin\n"
"                     * all the way round. */\n"
"                    hwi *= 1.0 - 0.16 * smoothstep(0.945, 0.995, t);\n"
"                    if (u_cflash > 0.5 && hwo > aa2)\n"
"                        c = mix(c, u_caccent, smoothstep(hwo + aa2, hwo - aa2, db));\n"
"                    if (hwi > aa2)\n"
"                        c = mix(c, pane, smoothstep(hwi + aa2, hwi - aa2, db));\n"
"                }\n""                if (u_cpflip > 0.5) {\n"
"                    /* THE VIKING INTERLOCK, read off the reference top to\n"
"                     * bottom: an upper PEARL FIELD with a birdseye spear\n"
"                     * rising through it, a waist where twin timber spikes\n"
"                     * climb into the pearl, then the pearl SPEAR descending\n"
"                     * into the birdseye below — every boundary black-lined.\n"
"                     * Two opposed spears sharing a waist, not one shape. */\n"
"                    float T0 = bs_tip, T3 = bs_base;\n"
"                    float Tm = mix(T0, T3, 0.38);\n"
"                    float aap = fwidth(d) * 1.2;\n"
"                    vec3 ink = vec3(0.05, 0.045, 0.045);\n""                    /* longitudinal bands take the LONGITUDINAL pixel width:\n"
"                     * fwidth(d) at grazing angles smeared these rings into a\n"
"                     * black wash over the whole cap. */\n"
"                    float aat = fwidth(t) * 1.2;\n"
"                    float rg2 = min(min(abs(t - T0), abs(t - (T3 + 0.012))),\n"
"                                    abs(t - (T3 + 0.030)));\n"
"                    c = mix(c, ink, 1.0 - smoothstep(0.004, 0.004 + aat, rg2));\n"
"                    if (t < Tm) {\n"
"                        /* pearl field, timber spear rising: pearl OUTSIDE.\n"
"                         * The field's own TOP closes as pearl spear-tips\n"
"                         * rising into the timber — nothing here stops on a\n"
"                         * straight line. */\n"
"                        float T0d = T0 - 0.070 * (1.0 - clamp(d / 0.45, 0.0, 1.0));\n"
"                        float ku = clamp((t - T0d) / (Tm - T0d), 0.0, 1.0);\n"
"                        float hs = 0.55 * pow(ku, 0.70);\n"
"                        c = mix(c, pane, smoothstep(hs - aap, hs + aap, d));\n"
"                        float lo = 1.0 - smoothstep(0.014, 0.014 + aap, abs(d - hs));\n"
"                        if (hs > aap) c = mix(c, ink, lo);\n"
"                        /* twin ARROWS climbing into the pearl at the waist.\n"
"                         * A real Viking spike is barbed: the tip flares into\n"
"                         * a pointed head, then the width STEPS back to a\n"
"                         * narrow shaft — the step corners are the barbs. */\n"
"                        float Ts   = Tm - 0.13;\n"
"                        float tspk = t - Ts;\n"
"                        float hl3  = 0.050;\n"
"                        float wsp  = (tspk < hl3) ? 0.075 * max(tspk, 0.0) / hl3 : 0.026;\n"
"                        float dsp  = abs(d - 0.30);\n"
"                        if (tspk > 0.0 && wsp > aap && d > hs) {\n"
"                            c = mix(c, u_cbutt, smoothstep(wsp + aap, wsp - aap, dsp));\n"
"                            float lo2 = 1.0 - smoothstep(0.010, 0.010 + aap, abs(dsp - wsp));\n"
"                            c = mix(c, ink, lo2);\n"
"                        }\n"
"                        /* the crown chevron between the arrows: a black line\n"
"                         * rising to its point at the face centre */\n"
"                        float tcr = Tm - 0.055 * (1.0 - clamp(d / 0.30, 0.0, 1.0));\n"
"                        float cr  = 1.0 - smoothstep(0.009, 0.009 + fwidth(t) * 1.2, abs(t - tcr));\n"
"                        if (d < 0.32 && d > hs) c = mix(c, ink, cr);\n"
"                    } else {\n"
"                        /* the pearl spear descending into the birdseye */\n"
"                        float kd = clamp((t - Tm) / (T3 - Tm), 0.0, 1.0);\n"
"                        float hd = 0.44 * pow(1.0 - kd, 1.05);\n"
"                        if (hd > aap) {\n"
"                            c = mix(c, pane, smoothstep(hd + aap, hd - aap, d));\n"
"                            float lo = 1.0 - smoothstep(0.014, 0.014 + aap, abs(d - hd));\n"
"                            c = mix(c, ink, lo);\n"
"                        }\n"
"                    }\n"
"                }\n"
"            }\n"
"\n"
"            /* ---- the timber's FIGURE ---------------------------------- *\n"
"             * What separates ebony from a photo of black: the butt woods\n"
"             * carry their own figure. */\n"
"            if (u_cbfig > 0.5 && t > ms_base) {\n"
"                float fang = a * 6.2831853;\n"
"                if (u_cbfig < 1.5) {          /* plain figure: ebony/rosewood */\n"
"                    c *= 0.90 + 0.20 * fbm2(vec2(t * 50.0, fang * 1.5));\n"
"                } else if (u_cbfig < 2.5) {   /* birdseye maple */\n"
"                    float e1  = vnoise(vec2(t * 420.0, fang * 9.0));\n"
"                    float eye = smoothstep(0.80, 0.92, e1);\n"
"                    c *= 0.92 + 0.16 * fbm2(vec2(t * 40.0, fang));\n"
"                    c = mix(c, c * 0.50, eye);\n"
"                } else if (u_cbfig < 3.5) {   /* curly maple: bright cross-bands */\n"
"                    float curl = 0.5 + 0.5 * sin(t * 220.0 + 4.0 * fbm2(vec2(t * 30.0, fang * 0.4)));\n"
"                    c *= 0.84 + 0.30 * curl;\n"
"                } else {                      /* wenge: straight dark stripes */\n"
"                    float st = fract(fang * 4.77 + 0.8 * fbm2(vec2(t * 12.0, fang)));\n"
"                    float sd = min(st, 1.0 - st);\n"
"                    c = mix(c, c * 0.45, 1.0 - smoothstep(0.10, 0.22, sd));\n"
"                }\n"
"            }\n"
"            /* ---- the AMERICAN structure ------------------------------- *\n"
"             * A linen wrap where the hand goes, a separate butt sleeve\n"
"             * finished with collar rings, and diamond inlays. These are\n"
"             * top-level: they exist whether or not the cue has points. */\n"
"            if (u_cwrap > 0.5 && t > 0.795 && t < 0.915) {\n"
"                float wsp = fract(sin(dot(vec2(a * 260.0, t * 1400.0),\n"
"                                          vec2(12.9898, 78.233))) * 43758.5453);\n"
"                float wth = fract(a * 130.0 + t * 40.0);\n"
"                c = u_cwrapc * (0.55 + 0.75 * wsp) * (0.85 + 0.30 * wth);\n"
"                gloss = 10.0; spec_k = 0.05;   /* linen is matt, not lacquer */\n"
"                butt_varn = 0.0;\n"
"            }\n"
"            if (u_csleeve > 0.5) {\n"
"                if (t > ((u_cwrap > 0.5) ? 0.920 : 0.845)) c = u_csleevec;\n"
"                /* collar RINGS come in groups — a lone hairline reads as an\n"
"                 * accident, the grouped pinstripes of the references read as\n"
"                 * design. Ring + companion at both wrap ends and the cap. */\n"
"                float rg = 1e9;\n"
"                float W1 = (u_cwrap > 0.5) ? 0.918 : 0.845;\n"
"                float W0 = (u_cwrap > 0.5) ? 0.782 : 0.612;\n"
"                rg = min(rg, abs(t - W1)); rg = min(rg, abs(t - (W1 + 0.013)));\n"
"                rg = min(rg, abs(t - W0)); rg = min(rg, abs(t - (W0 - 0.013)));\n"
"                rg = min(rg, abs(t - 0.965));\n"
"                /* a machined ring edge is CRISP — the fixed soft fade read as\n"
"                 * blur. Full strength across the ring, one pixel of AA off it. */\n"
"                float aar = fwidth(t) * 1.2;\n"
"                c = mix(c, u_cringc, 1.0 - smoothstep(0.0026, 0.0026 + aar, rg));\n"
"            }\n"
"            /* Diamond inlays: a pale plate let into the timber with a\n"
"             * coloured core, one per point position. On the sleeve when\n"
"             * there is one, on the forearm when there is not. */\n"
"            if (u_cdia > 0.5) {\n"
"                float tc = (u_cit > 0.0) ? u_cit\n"
"                         : ((u_csleeve > 0.5) ? ((u_cwrap > 0.5) ? 0.947 : 0.895) : 0.545);\n"
"                float hl2 = (u_cishape > 2.5) ? 0.055 : 0.030;   /* spears run long */\n"
"                /* A framed diamond never travels alone: the references run\n"
"                 * them in rows of three down the sleeve or forearm. */\n"
"                float md = 1e9;\n"
"                int nrow = (u_cishape > 1.5 && u_cishape < 2.5) ? 1 : 0;\n"
"                float mdm = 1e9;   /* the small companions between the majors */\n"
"                for (int di2 = -nrow; di2 <= nrow; di2++) {\n"
"                    float pitch = (u_cwrap > 0.5) ? 0.040 : 0.068;\n"
"                    float m1 = abs(t - tc - float(di2) * pitch) / hl2 + d / 0.20;\n"
"                    md = min(md, m1);\n"
"                    if (di2 < nrow) {\n"
"                        float m2 = abs(t - tc - (float(di2) + 0.5) * pitch) / (hl2 * 0.45)\n"
"                                 + d / 0.09;\n"
"                        mdm = min(mdm, m2);\n"
"                    }\n"
"                }\n"
"                md = min(md, mdm);\n"
"                float pl = 1.0 - smoothstep(1.00, 1.08, md);\n"
"                float co = 1.0 - smoothstep(0.66, 0.78, md);\n"
"                vec3 icol = u_cdiac;\n"
"                if (u_cipearl > 0.5) {\n"
"                    /* pearl: iridescent sheet, banded, catching to white */\n"
"                    float sh = fbm2(vec2(t * 160.0, a * 30.0));\n"
"                    icol = mix(u_cdiac * 0.75, vec3(1.0), smoothstep(0.35, 0.85, sh));\n"
"                }\n"
"                c = mix(c, u_cvnr2, pl);              /* the let-in plate */\n"
"                if (u_cishape > 1.5 && u_cishape < 2.5) {\n"
"                    float fr = 1.0 - smoothstep(0.88, 0.98, md);   /* dark frame */\n"
"                    c = mix(c, vec3(0.06, 0.05, 0.05), fr);\n"
"                }\n"
"                c = mix(c, icol, co);\n"
"            }\n"
"\n"
"            /* 3. the badge: a round ivory plate on the very end, and the black\n"
"             *    band the cap is finished with. */\n"
"            // NO BADGE. A maker's disc belongs on the flat oval of a butt cap,\n"
"            // and this butt is a surface of revolution — so it was being\n"
"            // wrapped round a curve, which is not where a badge goes and looked\n"
"            // it. Better absent than wrong; it needs a flat end to sit on, and\n"
"            // that is a geometry change rather than a shader one.\n"
"            // A thin dark line at the very end, which is all a real butt cap\n"
"            // shows. It was 20 mm of black across the end of the cue, added to\n"
"            // frame a badge that has since gone, and it read as a stripe\n"
"            // painted on rather than as the end of the cue.\n"
"            if (t > 0.9865 && t < 0.9915) c = u_cbutt * 0.30;\n"
"            // Brass. A three-quarter jointed cue has a bright collar at the\n"
"            // joint and another at the butt cap, and both catch the light hard\n"
"            // enough to be a feature rather than a detail.\n"
"            float brass_lit = 0.0;\n"
"            // The butt collar sits just SHORT of the cap. Sitting on it, the\n"
"            // brass wrapped the rounded end and the cue finished in a gold\n"
"            // cone — a real one finishes in a dark rubber or horn cap with the\n"
"            // brass ring behind it.\n"
"            // BETWEEN the two splices, in the plain black — never through the\n"
"            // points. On a real cue the joint is a band of bare ebony with the\n"
"            // collar on it, and a collar crossing the splice is the one thing\n"
"            // that instantly reads as wrong.\n"
"            // 3 mm of collar. It was 0.645 to 0.660, which on a 1.45 m cue is\n"
"            // 22 mm — a band you could read a maker's name off, not a joint.\n"
"            if (t > 0.6480 && t < 0.6501) {\n"
"                // BRASS, treated as metal — the same way the ferrule is.\n"
"                //\n"
"                // A flat yellow with a Blinn highlight on it is what yellow\n"
"                // PLASTIC looks like, and that is exactly how it read. Metal\n"
"                // has almost no diffuse: nearly all its brightness is what it\n"
"                // is reflecting, so it has to go dark where it reflects nothing\n"
"                // and blow out where it catches a lamp. On a collar wrapped\n"
"                // round a shaft that gives the giveaway signature — bright\n"
"                // along the top, dark down the flank, and a bright rim where\n"
"                // the curve turns away.\n"
"                //\n"
"                // Brass rather than steel: warm, and the reflection is TINTED\n"
"                // by the metal instead of staying white, which is the whole\n"
"                // difference between a gold metal and a grey one.\n"
"                vec3 nn = normalize(v_nrm);\n"
"                vec3 Vv = normalize(v_eyepos - v_world);\n"
"                vec3 R  = reflect(-Vv, nn);\n"
"                float up   = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);\n"
"                float band = pow(up, 3.0);\n"
"                float rim  = pow(1.0 - abs(dot(nn, Vv)), 2.5);\n"
"                vec3 brass = vec3(0.72, 0.53, 0.19);\n"
"                c = brass * (0.30 + 1.45 * band)\n"
"                  + vec3(0.95, 0.82, 0.52) * rim * 0.42;\n"
"                brass_lit = 1.0;\n"
"            }\n"
"            gloss = mix(gloss, 160.0, brass_lit);\n"
"            spec_k = mix(spec_k, 0.85, brass_lit);\n"  // brass collar
"        }\n"
"        /* uv.y past 1.0 is the butt-end material channel: geometry says\n"
"         * what it is made of. 1.02 = the brass socket ring, 1.05 = the dark\n"
"         * threaded bore behind it, 1.08 = the badge disc on the flat. */\n"
"        if (t > 1.065) {\n"
"            c = (a > 0.90) ? vec3(0.22, 0.18, 0.15) : vec3(0.93, 0.90, 0.79);\n"
"            butt_varn = 0.0; gloss = 70.0; spec_k = 0.45;\n"
"        } else if (t > 1.035) {\n"
"            c = vec3(0.055, 0.045, 0.035);\n"
"            butt_varn = 0.0; gloss = 8.0; spec_k = 0.05;\n"
"        } else if (t > 1.005) {\n"
"            vec3 nn2 = normalize(v_nrm);\n"
"            vec3 Vv2 = normalize(v_eyepos - v_world);\n"
"            float up2 = clamp(reflect(-Vv2, nn2).y * 0.5 + 0.5, 0.0, 1.0);\n"
"            float rim2 = pow(1.0 - abs(dot(nn2, Vv2)), 2.0);\n"
"            c = vec3(0.72, 0.53, 0.19) * (0.45 + 1.1 * pow(up2, 2.0))\n"
"              + vec3(0.95, 0.82, 0.52) * rim2 * 0.40;\n"
"            butt_varn = 0.0; gloss = 160.0; spec_k = 0.85;\n"
"        }\n"
"        float d = diffuse(v_nrm, L);\n"
"        float spec = pow(max(dot(v_nrm, normalize(L + vec3(0.0, 0.0, 1.0))), 0.0), gloss);\n"
"        o_col = emit(to_linear(c) * (0.34 + 0.70 * d) + vec3(spec) * spec_k\n"
"                   + vec3(butt_varn), 1.0, 1.0);\n"
"    } else if (u_mode == 12) {\n"
"        // The frame's NON-timber pieces: brass, chrome, laminate, and the black\n"
"        // down a pocket. Vertex colour and one light, and above all no varnish\n"
"        // — timber()'s specular on a near-black surface is a grey square, which\n"
"        // is precisely what a pocket looked like when these went through the\n"
"        // wood shader with everything else.\n"
"        float d = diffuse(normalize(v_nrm), L);\n"
"        o_col = emit(to_linear(v_col) * (0.34 + 0.70 * d), 1.0, 1.0);\n"
"    } else if (u_mode == 11) {\n"
"        // A runtime-supplied controller model: a base-colour texture times a\n"
"        // factor, and one light. Deliberately plain — this is Meta's picture of\n"
"        // Meta's hardware and the job here is to show it, not to restyle it.\n"
"        // Parts with no texture are given a 1x1 white one so there is no branch.\n"
"        vec4 t = texture(u_tex, v_uv);\n"
"        vec3 c = to_linear(t.rgb) * u_colour.rgb;\n"
"        float d = diffuse(normalize(v_nrm), L);\n"
"        o_col = emit(c * (0.40 + 0.66 * d), 1.0, 1.0);\n"
"    } else if (u_mode == 4 || u_mode == 7) {\n"
"        // Debug: the AUTHORED colour, unshaded. Reading the code and guessing\n"
"        // which quad a stray patch is has now failed three times; this makes\n"
"        // the mesh say what it is, and the answer can be compared numerically\n"
"        // against the palette it was built from.\n"
"        if (u_rawcol > 0.5 && u_rawcol < 1.5) { o_col = vec4(v_col, 1.0); return; }\n"
"        // The table (4) and the body under it (7), through ONE wood path.\n"
"        //\n"
"        // They were two branches with a timber() call each. timber() is a big\n"
"        // function — rings, cathedral figure, knots, pores, ray fleck and an\n"
"        // anisotropic varnish — and a GLSL compiler inlines it at every call\n"
"        // site, so a second one very nearly doubled the fragment shader. A\n"
"        // desktop driver shrugs at that; a tiled mobile GPU has a bounded\n"
"        // instruction store and can simply refuse to link, and a program that\n"
"        // does not link draws NOTHING AT ALL — which in a passthrough headset\n"
"        // looks exactly like an app that failed to start.\n"
"        //\n"
"        // The two only ever differed in where the board coordinate comes from\n"
"        // and how much ambient the surface gets, so that is all that branches.\n"

"        // The table, with the handheld's own shading: colours authored per\n"
"        // triangle, lit by the ABSOLUTE dot with the overhead key. Absolute\n"
"        // because the mesh is double-sided — the cloth fan and the pocket\n"
"        // voids are wound for shading, not for a front-face convention.\n"
"        float ndl = diffuse_abs(normalize(v_nrm), L);\n"
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
"        // WHAT THE SURFACE IS, from the mesh. uv.x is 1 on cloth and 0 on\n"
"        // timber, stamped by cue_render at emit time.\n"
"        //\n"
"        // It used to compare the vertex colour's HUE to the cloth's and guess.\n"
"        // That is not a material system, it is a coincidence detector, and it\n"
"        // guessed wrong on the dark wood inside a pocket bore — running the\n"
"        // cloth's Charlie sheen over it, which ADDS a 30%-white lobe and turned\n"
"        // an authored (49,28,8) into (135,98,79). That was the pale square\n"
"        // beside every pocket on every table, and it survived three wrong\n"
"        // diagnoses because I kept looking for a stray quad instead of asking\n"
"        // what the shader thought it was drawing.\n"
"        //\n"
"        // The body (mode 7) is never cloth.\n"
"        float iscloth = u_mode == 7 ? 0.0 : v_uv.x;\n"
"        vec3 sn = normalize(v_nrm);\n"
"        if (iscloth > 0.01) {\n"
"            // Cloth is not varnished. With the plain setting the bed has no\n"
"            // sheen, so the cushions and the pocket throats must not either —\n"
"            // they were still running the full Charlie lobe, which is what made\n"
"            // the inside of a pocket look wet.\n"
"            if (u_clothlod < 0.5) {\n"
"                float dc = 0.30 + 0.70 * diffuse_abs(sn, L);\n"
"                o_col = emit(to_linear(v_col * dc), 1.0, 1.0);\n"
"                return;\n"
"            }\n"
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
"            tc = v_col * (0.30 + 0.70 * diffuse_abs(sn, L))\n"
"               * mix(1.0, mocc, iscloth);\n"
"            o_col = emit(to_linear(tc) + sc * sh * 0.85 * iscloth, 1.0, 1.0);\n"
"            return;\n"
"        }\n"
"        \n"
"        if (iscloth < 0.99) {\n"
"            // Board coordinates: along the rail and across it, on the surface itself.\n"
"            // Board axes, derived from the geometry.\n"
"            //\n"
"            // Two things wrong before. surf_uv builds its tangent from the NORMAL, so on a\n"
"            // horizontal rail top it returns (z, x) and along-the-board came out ACROSS\n"
"            // the table. And the replacement still took BOTH axes from the horizontal\n"
"            // plane, so on a vertical face the second axis barely changed with height —\n"
"            // which is why the sides came out as pure vertical streaks with no figure.\n"
"            //\n"
"            // A rail runs along whichever world axis it is long in. On a horizontal face\n"
"            // the second axis is the other horizontal one; on a vertical face it is HEIGHT,\n"
"            // because that is the direction across the board there.\n"
"            // The frame AUTHORS its grain coordinate — uv.x along whichever\n"
"            // length of timber the vertex belongs to and uv.y across it — which\n"
"            // is better than what the rails have to do below, where the board\n"
"            // axes have to be inferred from the geometry.\n"
"            vec2 wq = v_uv;\n"
"            vec3 tang = vec3(1.0, 0.0, 0.0);\n"
"            if (u_mode == 4) {\n"
"                vec3 nn = normalize(v_nrm);\n"
"                if (abs(nn.y) > 0.7) {\n"
"                    // Which rail am I standing on? NOT |x| vs |z| — a table is twice\n"
"                    // as long as it is wide, so that test switches along the diagonal\n"
"                    // through the table CENTRE, which crosses the side rails about a\n"
"                    // third of the way down and turns the grain there. Compare the\n"
"                    // distance to each pair of edges instead: the locus where those\n"
"                    // are equal is the 45 degree bisector out of the corner, which is\n"
"                    // exactly where a real frame is mitred. rail_w is the same all\n"
"                    // round, so that one line passes through the inner corner and the\n"
"                    // outer corner both.\n"
"                    vec2 d = u_half - abs(v_local.xz);\n"
"                    vec2 a2 = (d.y < d.x) ? vec2(1.0, 0.0)   // side rail: along x\n"
"                                          : vec2(0.0, 1.0);  // end rail:  along z\n"
"                    wq = vec2(dot(v_local.xz, a2), dot(v_local.xz, vec2(-a2.y, a2.x)));\n"
"                    tang = vec3(a2.x, 0.0, a2.y);\n"
"                } else {\n"
"                    vec2 a2 = normalize(vec2(-nn.z, nn.x) + 1e-6);\n"
"                    wq = vec2(dot(v_local.xz, a2), v_local.y);\n"
"                    tang = vec3(a2.x, 0.0, a2.y);\n"
"                }\n"
"            }\n"
"            // One low-frequency fetch for the turbulence, one high for grain and pores.\n"
"            vec2 warp = (texture(u_nap, wq / 0.55).rg - 0.5) * 2.0;\n"
"            vec2 fine = (texture(u_nap, vec2(wq.x / 0.22, wq.y / 0.020)).rg - 0.5) * 2.0;\n"
"            float varn = 0.0;\n"
"            tc = mix(tc, timber(v_col, wq, warp, fine, v_nrm, tang, L, 105.0, varn),\n"
"                     1.0 - iscloth);\n"
"            // NO VARNISH ON THE TABLE'S WOODWORK.\n"
"            //\n"
"            // Rail stock is oiled and rubbed back, not french polished, and on\n"
"            // the horizontal faces the lobe was contributing nothing you could\n"
"            // see EXCEPT an artefact: a bright patch on the rail top around\n"
"            // every pocket. Square, because the bore is notched out of the\n"
"            // plank as a bounding box and the triangles bordering it form a\n"
"            // square ring; bright, because the crease-smoothed normals there\n"
"            // tilt across the branch the tangent used to be chosen by. Taking\n"
"            // the grain direction from the caller fixed most of it and not all,\n"
"            // and a highlight that only ever showed up as a bug is not worth\n"
"            // three more attempts. The frame body (mode 7) keeps its sheen —\n"
"            // an apron IS polished.\n"
"            wood_spec = varn * (1.0 - iscloth);\n"
"        }\n"
"        \n"
"        // Specular ADDED after the diffuse, not folded into it.\n"
"        //\n"
"        // The body gets a higher ambient floor than the rails, and it is not a\n"
"        // fudge: the rails lie under the lamps and the body does not. Every\n"
"        // face of an apron or a leg is vertical or downward, so almost none of\n"
"        // the key reaches it and what lights it is bounce — off the floor, off\n"
"        // the walls, off the cloth. At the rails' 0.32 the whole body went to\n"
"        // near-black and took the figure with it.\n"
"        float amb = u_mode == 7 ? 0.46 : 0.32;\n"
"        o_col = emit(to_linear(tc * (amb + (1.0 - amb) * ndl)) + vec3(wood_spec),\n"
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
"        // PLAIN CLOTH. The nap is the most expensive thing on screen — two\n"
"        // gradient-capped fetches, a Charlie sheen lobe, micro-occlusion and\n"
"        // two value-noise calls, over the largest area in the frame — and at\n"
"        // playing distance very little of it survives. This path keeps the\n"
"        // colour, the lighting and the chalk, and drops the rest.\n"
"        if (u_clothlod < 0.5) {\n"
"            float m0 = mark_cov(q, aa);\n"
"            // Through diffuse_abs, so the bed RECEIVES SHADOW. Computing the\n"
"            // lambert term inline here bypassed g_sh entirely, which is why the\n"
"            // cloth — the one surface every shadow lands on — had none.\n"
"            float d0 = 0.30 + 0.70 * diffuse_abs(nv, L);\n"
"            vec3 c0 = mix(to_linear(u_cloth) * d0, to_linear(u_markc), m0 * 0.88);\n"
"            o_col = emit(c0, 1.0, 1.0);\n"
"            return;\n"
"        }\n"
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
"        float ndl = diffuse_abs(N, L);\n"
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
"        float ndl = diffuse_abs(nv, L);\n"
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
"        // u_colour.a is this blob's share: a rig with six lamps casts six\n"
"        // shadows and each one has to be correspondingly faint.\n"
"        //\n"
"        // The EDGE comes from the rig. Every mode was using one very soft\n"
"        // falloff — smoothstep from 0.10, so the blob faded across nine tenths\n"
"        // of its own width — which made a bar of hard shades over a table look\n"
"        // like an overcast afternoon. A small source close overhead throws an\n"
"        // edge you could cut yourself on; only the window should be woolly.\n"
"        float inner = 1.0 - u_shadow.x;\n"
"        float a = u_shadow.y * u_colour.a * (1.0 - smoothstep(inner, 1.0, d));\n"
"        o_col = emit(to_linear(u_clothsh) * 0.55, a, 0.0);\n"
"    } else {\n"
"        vec3 c = to_linear(u_colour.rgb);\n"
"        float d = diffuse(v_nrm, L);\n"
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
    GLint  u_cloth, u_fur, u_nap, u_feltspan, u_half, u_furslice, u_furslices, u_furdbg, u_shell,
           u_cshaft, u_csplice, u_caccent, u_cbutt, u_cburr, u_cflash, u_cvnr2, u_cvw, u_cv2on, u_cdiac, u_cdia, u_cvcol, u_cnvcol, u_csfig, u_cbfig, u_cishape, u_cipearl, u_cit, u_chand, u_cnarch, u_cpflip, u_cppearl,
           u_cwrapc, u_csleevec, u_cringc, u_cpts, u_cptlen, u_cnvnr, u_cwrap, u_csleeve, u_clam, u_markc, u_baulk, u_drad, u_linew, u_spotr, u_nspot, u_spots;
    GLint  u_lampC, u_lampX, u_lampZ, u_lampG, u_lampN, u_lampI, u_nlamp, u_lampround, u_eye;
    GLint  u_keyc, u_fill, u_hudv, u_hudrect, u_shadow, u_clothlod, u_rawcol, u_varn;
    int    minimal;            /* the real shader would not build; see FS_MIN */
    /* The runtime's own controller models, when XR_FB_render_model gives them.
     * They supersede the baked STLs — including the model-to-grip matrix below,
     * which stops being a guess because the runtime authors its model in the
     * grip frame. */
    struct {
        Mesh   mesh;
        int    nparts;
        struct { int first, count, tex; float base[4]; } part[CUEVR_GLB_MAX_PARTS];
        GLuint tex[CUEVR_GLB_MAX_PARTS];
        int    ntex;
        int    have;
    } rm[2];
    CueVrLightRig rig;
    int    light_mode;
    MoteVrV3 key_room;
    Mesh   ctrl[2];
    Mesh   fins, bed, table, lips, frame, ball, cue, quad, floor, grip;
    int    frame_sel;          /* -1 = whichever design suits the table */
    int    frame_timber_n;
    GLuint sh_fbo, sh_tex, sh_prog;
    GLint  sh_u_lightvp, sh_u_model;
    GLint  u_shmap, u_shmat, u_shon, u_shtexel, u_shsoft, u_norefl;
    int    sh_size;     /* indices of the frame that are wood; see cuevr_frame.h */
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
/* Compile the multiview variant. Set from mote_xr_multiview() BEFORE
 * cuevr_render_init, and it is not optional: a vertex shader with no
 * `layout(num_views = 2) in;` drawing into a multiview framebuffer is an
 * INVALID_OPERATION and the draw is discarded — so the whole game renders
 * nothing at all and the headset shows bare passthrough. It sat here declared
 * and never assigned, which is exactly that. */
static int s_mv_shader;
void cuevr_render_set_multiview(int on) { s_mv_shader = on ? 1 : 0; }

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
    /* 4 KB, not 1: a mobile driver's link failure comes back as a paragraph and
     * the reason is usually at the END of it. Truncating the one message that
     * explains the failure is not a saving. */
    if (!ok) { char l[4096]; glGetShaderInfoLog(s, sizeof l, NULL, l); LOGI("[cuevr] shader: %s", l); return 0; }
    return s;
}

/* ---- the shadow map ------------------------------------------------------ *
 *
 * One depth pass from the key light, orthographic over the table. It replaces
 * the blob decals entirely: a decal is a guess at where a shadow would be, and
 * it cannot put the cue's shadow on the cloth, cannot let one ball shade another
 * in the pack, and cannot shade a ball against the cushion it is frozen on.
 *
 * View-INDEPENDENT, so it is rendered once a frame rather than once per eye —
 * multiview does not double it.
 *
 * Deliberately low resolution. The usual reason shadow mapping gets expensive is
 * chasing crisp edges with 9- or 16-tap filtering, and crisp is exactly what a
 * cue sports table must not have: it is lit by wide sources close overhead, so
 * the shadow under a ball is small and soft. At 1024 over a 12 ft table a texel
 * is 3.5 mm, and a single hardware PCF fetch through sampler2DShadow gives a
 * penumbra of about that — which is the look, arrived at by being cheap. */
static const char *VS_SHADOW =
"layout(location=0) in vec3 a_pos;\n"
"uniform mat4 u_lightvp;\n"
"uniform mat4 u_model;\n"
"void main() { gl_Position = u_lightvp * u_model * vec4(a_pos, 1.0); }\n";

static const char *FS_SHADOW =
"precision highp float;\n"
"void main() { }\n";

/* The fallback fragment shader.
 *
 * If the real one will not compile or link, the app used to draw NOTHING — and
 * in a passthrough headset an app that draws nothing is indistinguishable from
 * an app that did not start. There is no console to look at and no way to tell
 * a shader problem from a crash, which cost an entire debugging session.
 *
 * So there is always a program. This one is flat vertex colours and a single
 * light, which is ugly and unmistakably wrong — you can see at a glance that
 * the real shader failed, and everything else in the game still works while
 * somebody reads the log. Every uniform the draw code sets that this does not
 * declare resolves to -1, and glUniform on -1 is a defined no-op, so the
 * drawing code needs no knowledge of which program is bound. */
static const char *FS_MIN =
"precision highp float;\n"
"in vec3 v_nrm;\n"
"in vec2 v_uv;\n"
"in vec3 v_col;\n"
"uniform sampler2D u_tex;\n"
"uniform int  u_mode;\n"
"uniform vec4 u_colour;\n"
"out vec4 o_col;\n"
"void main() {\n"
"    if (u_mode == 2) {\n"
"        vec4 t = texture(u_tex, v_uv);\n"
"        o_col = vec4(t.rgb, t.a * u_colour.a);\n"
"        return;\n"
"    }\n"
"    float d = max(dot(normalize(v_nrm), normalize(vec3(0.15, 0.95, 0.20))), 0.0);\n"
"    o_col = vec4(v_col * (0.35 + 0.65 * d), 1.0);\n"
"}\n";

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
    /* The taper. A cue is not a stick that happens to be pointed: the whole
     * middle of it is one long cone, and how fast that cone opens is most of
     * what makes it read as a cue rather than as a dowel.
     *
     * 29 mm at the butt is within the range a real snooker cue is made in, but
     * at the thin end of it, and it looked thin. 32 mm is a full-handed butt —
     * and the joint has to come up with it or the extra only appears in the last
     * 30 cm and the cue looks like a stick with a knob on. So the joint goes
     * from 19 to 21 mm too, which steepens the shaft's taper by about a quarter
     * over its whole 1.07 m. */
    /* ONE LINEAR TAPER, ferrule to butt.
     *
     * It used to be two segments with different rates and then a rounded cap on
     * the end, which made the last 40 mm swell — and widening the butt only made
     * the swelling worse, because a bigger radius on the same hemisphere is a
     * bigger bulb. A cue is a straight cone: it gets steadily thicker all the way
     * and then stops. The end is closed by a flat cap on the mesh, so nothing
     * here needs to round it over. */
    if (x < 1.4440f) {
        float k = (x - 0.032f) / (1.4440f - 0.032f);
        if (k < 0.0f) k = 0.0f;
        return 0.0051f + k * (0.0160f - 0.0051f);
    }
    /* THE BUTT CAP, nearly flat.
     *
     * The taper above reaches the cue's MAX WIDTH at 1.410 and the last 40 mm
     * round over from there. The max is where the cap BEGINS, not the very end
     * of the stick — running the linear taper all the way to 1.45 made the whole
     * cue a plain cone with a chopped-off end. */
    /* The reference butt end is nearly CYLINDRICAL: the taper runs to a few
     * millimetres from the end and a small chamfer breaks the edge — not the
     * bulbous dome this used to be. The end face carries the brass socket. */
    if (x < 1.4440f) {
        float k2 = (x - 0.032f) / (1.4440f - 0.032f);
        return 0.0051f + k2 * (0.0160f - 0.0051f);
    }
    float k = (x - 1.4440f) / (CUE_LEN - 1.4440f);
    if (k > 1.0f) k = 1.0f;
    return 0.0160f * sqrtf(1.0f - k * k * 0.16f);
}

/* A real cue is NOT a solid of revolution. The butt of a hand-spliced cue carries
 * a FLAT down one side — it is planed off so the badge plate can be let into it, and
 * it is also what stops the cue rolling off the table and tells your hand which way
 * up it is. Lathing a perfect cylinder is the giveaway that a cue was turned by a
 * computer rather than made.
 *
 * The flat starts where the splice ends and runs to the butt cap, deepening as the
 * cue thickens so its width stays roughly constant. */
/* PLAIN ROUND. There used to be a planed flat down one side of the butt from the
 * end of the splice to the cap — a real hand-spliced cue has one, so the badge
 * can be let in and so it does not roll off the table. Drawn on a shape this
 * size it reads as a dent rather than as a facet, and with the badge gone it has
 * nothing to be there for. Kept as a function rather than deleted so the shape
 * is still described in one place if it is ever wanted back. */
/* THE FLAT. The butt of a spliced cue is planed off down one side so the
 * badge has a face to sit on. Geometrically: a plane at distance p from the
 * axis, so within its angular span the radius is p / cos(dtheta) — a true
 * chord, not a dent. Depth ramps in over the last 300 mm and reaches 22% of
 * the radius at the cap; the flat faces AWAY from the decorative panel. */
#define CUE_FLAT_ANG   (0.625f * 2.0f * PI)     /* opposite the panel at 0.125 */
#define CUE_FLAT_DEPTH 0.22f
static float cue_flat_depth(float t) {
    /* REMOVED at the user's instruction: it never read on screen, so it was
     * dead geometry complicating everything that sat on it. */
    (void)t; return 0.0f;
}
static float cue_flat_scale(float t, float ang) {
    (void)t; (void)ang;
    return 1.0f;
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
    /* CLOSE THE END — as the real thing is closed (image 34): the timber cap
     * carries a BRASS EXTENSION SOCKET recessed into its centre, a bright ring
     * with a dark threaded bore behind it. uv.y past 1.0 is the material
     * channel: 1.02 brass, 1.05 bore, 1.08 badge — geometry says what it is
     * made of, nothing is texture-mapped on. */
    {
        float rend = cue_radius(1.0f);
        const float RB = 0.0092f;     /* brass ring outer radius */
        const float RI = 0.0058f;     /* bore radius */
        const float BD = 0.011f;      /* bore depth */
        /* timber annulus rend -> RB */
        for (int i = 0; i < slices; i++) {
            float u0 = (float)i / slices, u1 = (float)(i + 1) / slices;
            float t0 = u0 * 2.0f * PI, t1 = u1 * 2.0f * PI;
            float f0 = cue_flat_scale(1.0f, t0), f1 = cue_flat_scale(1.0f, t1);
            int aa = b_vert(b, cosf(t0) * rend * f0, CUE_LEN, sinf(t0) * rend * f0, 0, 1, 0, u0, 0.999f);
            int bb = b_vert(b, cosf(t1) * rend * f1, CUE_LEN, sinf(t1) * rend * f1, 0, 1, 0, u1, 0.999f);
            int cc = b_vert(b, cosf(t1) * RB, CUE_LEN, sinf(t1) * RB, 0, 1, 0, u1, 0.999f);
            int dd = b_vert(b, cosf(t0) * RB, CUE_LEN, sinf(t0) * RB, 0, 1, 0, u0, 0.999f);
            b_quad(b, aa, dd, cc, bb);
        }
        /* brass ring RB -> RI */
        for (int i = 0; i < slices; i++) {
            float u0 = (float)i / slices, u1 = (float)(i + 1) / slices;
            float t0 = u0 * 2.0f * PI, t1 = u1 * 2.0f * PI;
            int aa = b_vert(b, cosf(t0) * RB, CUE_LEN, sinf(t0) * RB, 0, 1, 0, u0, 1.02f);
            int bb = b_vert(b, cosf(t1) * RB, CUE_LEN, sinf(t1) * RB, 0, 1, 0, u1, 1.02f);
            int cc = b_vert(b, cosf(t1) * RI, CUE_LEN, sinf(t1) * RI, 0, 1, 0, u1, 1.02f);
            int dd = b_vert(b, cosf(t0) * RI, CUE_LEN, sinf(t0) * RI, 0, 1, 0, u0, 1.02f);
            b_quad(b, aa, dd, cc, bb);
        }
        /* bore wall + floor, recessed */
        for (int i = 0; i < slices; i++) {
            float u0 = (float)i / slices, u1 = (float)(i + 1) / slices;
            float t0 = u0 * 2.0f * PI, t1 = u1 * 2.0f * PI;
            int aa = b_vert(b, cosf(t0) * RI, CUE_LEN, sinf(t0) * RI, -cosf(t0), 0, -sinf(t0), u0, 1.05f);
            int bb = b_vert(b, cosf(t1) * RI, CUE_LEN, sinf(t1) * RI, -cosf(t1), 0, -sinf(t1), u1, 1.05f);
            int cc = b_vert(b, cosf(t1) * RI, CUE_LEN - BD, sinf(t1) * RI, -cosf(t1), 0, -sinf(t1), u1, 1.05f);
            int dd = b_vert(b, cosf(t0) * RI, CUE_LEN - BD, sinf(t0) * RI, -cosf(t0), 0, -sinf(t0), u0, 1.05f);
            b_quad(b, aa, bb, cc, dd);
        }
        {
            int centre = b_vert(b, 0.0f, CUE_LEN - BD, 0.0f, 0, 1, 0, 0.0f, 1.05f);
            for (int i = 0; i < slices; i++) {
                float u0 = (float)i / slices, u1 = (float)(i + 1) / slices;
                float t0 = u0 * 2.0f * PI, t1 = u1 * 2.0f * PI;
                int v0 = b_vert(b, cosf(t0) * RI, CUE_LEN - BD, sinf(t0) * RI, 0, 1, 0, 0.9f, 1.05f);
                int v1 = b_vert(b, cosf(t1) * RI, CUE_LEN - BD, sinf(t1) * RI, 0, 1, 0, 0.9f, 1.05f);
                b_tri(b, centre, v1, v0);
            }
        }
    }
    /* THE BADGE: a separate disc STUCK ON the flat — real geometry, slightly
     * proud, its own rim — not a picture mapped into the wood. uv.x carries
     * the radial fraction so the shader can draw the rim ring. */
    {
        const float BR = 0.0078f;                    /* badge radius */
        const float TB = 1.0f - 0.055f / CUE_LEN;    /* centre, 55 mm from the end */
        float r0 = cue_radius(TB);
        float p  = r0 + 0.0006f;      /* let in nearly flush on the round */
        float nx = cosf(CUE_FLAT_ANG), nz = sinf(CUE_FLAT_ANG);
        float cxp = nx * p, cy = TB * CUE_LEN, czp = nz * p;
        float txx = -nz, tzz = nx;                   /* tangent across the flat */
        int centre = b_vert(b, cxp, cy, czp, nx, 0, nz, 0.0f, 1.08f);
        int ring0 = -1, prev = -1;
        const int BN = 20;
        for (int i = 0; i <= BN; i++) {
            float th = (float)i / BN * 2.0f * PI;
            float ox = cosf(th) * BR, oy = sinf(th) * BR;
            int v = b_vert(b, cxp + txx * ox, cy + oy, czp + tzz * ox,
                           nx, 0, nz, 1.0f, 1.08f);
            if (i == 0) ring0 = v;
            else b_tri(b, centre, prev, v);
            prev = v;
        }
        (void)ring0;
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
                /* uv.x carries the MATERIAL. The table mesh derives its board
                 * coordinates from v_local, so uv is free — and the shader must
                 * be told what a surface is rather than inferring it from the
                 * colour, which is what put a pale square beside every pocket. */
                idx[k] = b_vert(b, t->v[k].x, t->v[k].y, t->v[k].z,
                                t->nrm.x, t->nrm.y, t->nrm.z,
                                t->mat == CUE_MAT_CLOTH ? 1.0f : 0.0f, 0.0f);
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
                            nn.x, nn.y, nn.z,
                            t->mat == CUE_MAT_CLOTH ? 1.0f : 0.0f, 0.0f);
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
  /* Three collections, authored against three references, cue by cue. Not
   * fourteen colourways of one model: the struct is a small program — points,
   * laminate palette, timber figure, wrap, sleeve, inlays — and each entry
   * uses a different subset of it. */

  /* ---- Peradon: studied cue by cue against the catalogue ----------------- *
   * CLASSIC has a splice after all — a rounded mahogany arch, no veneer.
   * CENTURY and ASCOT hold a ROSEWOOD panel inside the arch, not black.
   * JOE DAVIS is the wide cream-bordered dome. Veneer widths vary by model. */
  { .name="CLASSIC",   .shaft={0.87f,0.75f,0.55f}, .splice={0.42f,0.16f,0.09f},
    .accent={0.42f,0.16f,0.09f}, .burr={0.44f,0.17f,0.10f}, .butt={0.42f,0.16f,0.09f},
    .points=4, .point_len=0.80f, .veneer_w=0.0010f, .butt_fig=1, .hand=1 },
  { .name="CROWN",     .shaft={0.87f,0.75f,0.55f}, .splice={0.24f,0.09f,0.07f},
    .accent={0.80f,0.14f,0.16f}, .burr={0.30f,0.11f,0.08f}, .butt={0.24f,0.09f,0.07f},
    .flash=1, .veneer_w=0.0012f,
    .points=4, .point_len=0.70f, .veneers=1, .butt_fig=1, .hand=1, .arches=2 },
  { .name="EDWARDIAN", .shaft={0.87f,0.75f,0.55f}, .splice={0.075f,0.060f,0.052f},
    .accent={0.075f,0.060f,0.052f}, .burr={0.095f,0.085f,0.080f}, .butt={0.075f,0.060f,0.052f},
    .points=4, .point_len=0.75f, .veneer_w=0.0010f, .butt_fig=1, .hand=1 },
  { .name="JOE DAVIS", .shaft={0.87f,0.75f,0.55f}, .splice={0.070f,0.058f,0.052f},
    .accent={0.93f,0.89f,0.76f}, .burr={0.11f,0.10f,0.095f}, .butt={0.070f,0.058f,0.052f},
    .flash=1, .veneer_w=0.0019f,
    .points=4, .point_len=1.10f, .veneers=1, .butt_fig=1, .hand=1, .arches=2 },
  { .name="CENTURY",   .shaft={0.86f,0.74f,0.54f}, .splice={0.070f,0.058f,0.052f},
    .accent={0.12f,0.62f,0.60f}, .burr={0.36f,0.14f,0.09f}, .butt={0.070f,0.058f,0.052f},
    .flash=1, .vnr2={0.93f,0.90f,0.80f}, .flash2=1, .veneer_w=0.0013f,
    .points=4, .point_len=1.05f, .veneers=2, .butt_fig=1, .hand=1, .arches=2 },
  { .name="ASCOT",     .shaft={0.87f,0.75f,0.55f}, .splice={0.075f,0.060f,0.052f},
    .accent={0.94f,0.90f,0.78f}, .burr={0.42f,0.17f,0.10f}, .butt={0.075f,0.060f,0.052f},
    .flash=1, .veneer_w=0.0014f,
    .points=4, .point_len=0.95f, .veneers=1, .butt_fig=1, .hand=1, .arches=2 },
  { .name="ROYAL",     .shaft={0.87f,0.75f,0.55f}, .splice={0.070f,0.058f,0.052f},
    .accent={0.93f,0.90f,0.80f}, .burr={0.10f,0.10f,0.10f}, .butt={0.070f,0.058f,0.052f},
    .flash=1, .veneer_w=0.0009f, .points=4, .point_len=1.35f, .veneers=1,
    .butt_fig=1, .hand=1 },

  /* ---- Taylor Made: laminated splices ----------------------------------- */
  { .name="TM RAINBOW",.shaft={0.86f,0.74f,0.54f}, .splice={0.055f,0.050f,0.048f},
    .accent={0.94f,0.92f,0.85f}, .burr={0.06f,0.05f,0.05f}, .butt={0.055f,0.050f,0.048f},
    .flash=1, .vnr2={0.85f,0.15f,0.15f}, .flash2=1, .veneer_w=0.0022f,
    .points=4, .point_len=1.00f, .veneers=2, .butt_fig=1, .hand=1, .nvcol=6,
    .vcol={{0.85f,0.15f,0.15f},{0.95f,0.55f,0.10f},{0.90f,0.80f,0.20f},
           {0.20f,0.65f,0.30f},{0.15f,0.35f,0.80f},{0.70f,0.25f,0.60f}} },
  { .name="TM TEAL",   .shaft={0.86f,0.74f,0.54f}, .splice={0.055f,0.050f,0.048f},
    .accent={0.94f,0.92f,0.85f}, .burr={0.06f,0.05f,0.05f}, .butt={0.055f,0.050f,0.048f},
    .flash=1, .vnr2={0.10f,0.58f,0.54f}, .flash2=1, .veneer_w=0.0022f,
    .points=4, .point_len=1.00f, .veneers=2, .butt_fig=1, .hand=1, .nvcol=3,
    .vcol={{0.10f,0.58f,0.54f},{0.09f,0.50f,0.47f},{0.94f,0.92f,0.85f}} },
  { .name="TM CORAL",  .shaft={0.86f,0.74f,0.54f}, .splice={0.055f,0.050f,0.048f},
    .accent={0.94f,0.92f,0.85f}, .burr={0.06f,0.05f,0.05f}, .butt={0.055f,0.050f,0.048f},
    .flash=1, .vnr2={0.93f,0.42f,0.34f}, .flash2=1, .veneer_w=0.0022f,
    .points=4, .point_len=1.00f, .veneers=2, .butt_fig=1, .hand=1, .nvcol=3,
    .vcol={{0.93f,0.42f,0.34f},{0.94f,0.92f,0.85f},{0.10f,0.09f,0.09f}} },
  { .name="TM OCEAN",  .shaft={0.86f,0.74f,0.54f}, .splice={0.055f,0.050f,0.048f},
    .accent={0.94f,0.92f,0.85f}, .burr={0.06f,0.05f,0.05f}, .butt={0.055f,0.050f,0.048f},
    .flash=1, .vnr2={0.15f,0.38f,0.78f}, .flash2=1, .veneer_w=0.0022f,
    .points=4, .point_len=1.00f, .veneers=2, .butt_fig=1, .hand=1, .nvcol=3,
    .vcol={{0.15f,0.38f,0.78f},{0.13f,0.33f,0.70f},{0.94f,0.92f,0.85f}} },

  /* ---- American ---------------------------------------------------------- */
  { .name="PREDATOR",  .shaft={0.90f,0.83f,0.65f}, .splice={0.16f,0.10f,0.07f},
    .accent={0.93f,0.88f,0.74f}, .burr={0.22f,0.13f,0.08f}, .butt={0.16f,0.10f,0.07f},
    .flash=1, .vnr2={0.93f,0.88f,0.74f}, .veneer_w=0.0013f,
    .points=4, .point_len=0.80f, .veneers=1, .shaft_fig=1, .butt_fig=4,
    .diamonds=1, .diac={0.85f,0.45f,0.12f}, .inlay_shape=2, .inlay_t=0.55f },
  { .name="GC RED",    .shaft={0.90f,0.82f,0.62f}, .splice={0.55f,0.09f,0.09f},
    .accent={0.95f,0.92f,0.84f}, .burr={0.62f,0.11f,0.10f}, .butt={0.55f,0.09f,0.09f},
    .flash=1, .vnr2={0.95f,0.92f,0.84f}, .flash2=1, .veneer_w=0.0013f,
    .points=4, .point_len=1.20f, .veneers=2, .shaft_fig=1, .butt_fig=1,
    .wrap=1, .wrapc={0.16f,0.16f,0.17f}, .sleeve=1, .sleevec={0.55f,0.09f,0.09f},
    .ringc={0.82f,0.66f,0.26f}, .diamonds=1, .diac={0.95f,0.92f,0.84f} },

  { .name="GC BLUE",   .shaft={0.90f,0.82f,0.62f}, .splice={0.13f,0.22f,0.62f},
    /* white-painted forearm, long machine-sharp blue spears outlined black,
     * per the detail reference: apexes to the tip, speckled wrap below */
    .accent={0.06f,0.06f,0.07f}, .burr={0.88f,0.86f,0.82f}, .butt={0.88f,0.86f,0.82f},
    .flash=1, .veneer_w=0.0012f, .points=4, .point_len=1.30f, .veneers=1,
    .shaft_fig=1, .butt_fig=0, .hand=0,
    .wrap=1, .wrapc={0.16f,0.16f,0.17f}, .sleeve=1, .sleevec={0.88f,0.86f,0.82f},
    .ringc={0.10f,0.10f,0.11f}, .diamonds=1, .diac={0.13f,0.22f,0.62f}, .arches=-1 },
  { .name="VIKING EYE",.shaft={0.90f,0.83f,0.65f}, .splice={0.30f,0.12f,0.08f},
    /* the two-timber Viking: dark rosewood upper butt giving way to birdseye
     * through the big pale panel, cream facing, framed red diamonds riding
     * the split, grouped pinstripe rings */
    .accent={0.94f,0.93f,0.90f}, .burr={0.78f,0.58f,0.34f}, .butt={0.30f,0.12f,0.08f},
    .flash=1, .veneer_w=0.0016f, .points=4, .point_len=2.2f, .veneers=1,
    .shaft_fig=1, .butt_fig=2, .hand=0, .arches=-1,
    .sleeve=1, .sleevec={0.78f,0.58f,0.34f}, .ringc={0.90f,0.91f,0.93f},
    .diamonds=1, .diac={0.70f,0.12f,0.14f}, .inlay_shape=2, .inlay_t=0.90f,
    .vnr2={0.06f,0.055f,0.05f} },
  { .name="VIKING PEARL",.shaft={0.90f,0.83f,0.65f}, .splice={0.88f,0.81f,0.63f},
    /* the Viking interlock: upper pearl field with a birdseye spear rising,
     * twin spikes at the waist, pearl spear descending below. point_len only
     * stretches the panel window; the prongs are shaft-on-shaft, invisible. */
    .accent={0.05f,0.045f,0.045f}, .burr={0.82f,0.70f,0.48f}, .butt={0.80f,0.68f,0.46f},
    .veneer_w=0.0012f, .points=4, .point_len=4.0f,
    .shaft_fig=1, .butt_fig=2, .panel_flip=1, .panel_pearl=1,
    .ringc={0.86f,0.87f,0.90f}, .diac={0.14f,0.36f,0.90f},
    .vnr2={0.05f,0.045f,0.045f} },
  { .name="VIKING BLUE",.shaft={0.90f,0.83f,0.65f}, .splice={0.13f,0.22f,0.50f},
    /* blue-stained curly maple with pale machine points and a row of framed
     * white-pearl diamonds on the sleeve */
    .accent={0.93f,0.93f,0.95f}, .burr={0.15f,0.25f,0.55f}, .butt={0.13f,0.22f,0.50f},
    .flash=1, .veneer_w=0.0012f, .points=4, .point_len=1.10f, .veneers=1,
    .shaft_fig=1, .butt_fig=1, .hand=0,
    .sleeve=1, .sleevec={0.14f,0.24f,0.52f}, .ringc={0.88f,0.89f,0.92f},
    .diamonds=1, .diac={0.93f,0.94f,0.96f}, .inlay_shape=2, .inlay_pearl=1,
    .vnr2={0.06f,0.06f,0.07f}, .arches=-1 },
};
#define CUE_RACK_N ((int)(sizeof CUE_RACK / sizeof CUE_RACK[0]))
static int s_cue_sel;

int         cuevr_render_cue_count(void) { return CUE_RACK_N; }
const char *cuevr_render_cue_name(int i) {
    return (i >= 0 && i < CUE_RACK_N) ? CUE_RACK[i].name : "";
}
void cuevr_render_set_cue(int i) { s_cue_sel = (i < 0 || i >= CUE_RACK_N) ? 0 : i; }

/* ---- the runtime's controller models -------------------------------------- *
 *
 * XR_FB_render_model gives the actual hardware in the player's hands, authored
 * in the controller's GRIP frame — which is the whole point. The baked STLs
 * needed a model-to-grip matrix that could not be derived, only looked at, and
 * they are the wrong shape the moment somebody plays on a different controller.
 *
 * A model arrives as glTF and comes apart into one draw per material, so it
 * keeps its own small vertex buffer rather than joining the scene's. */

static GLuint s_white;      /* 1x1, for parts with no texture */

static void rm_free(int h) {
    mesh_free(&G.rm[h].mesh);
    if (G.rm[h].ntex) glDeleteTextures(G.rm[h].ntex, G.rm[h].tex);
    memset(&G.rm[h], 0, sizeof G.rm[h]);
}

void cuevr_render_set_ctrl_model(int hand, const void *bytes, unsigned len) {
    if (hand < 0 || hand > 1 || !bytes || !len) return;
    CueVrGlbModel m;
    if (cuevr_glb_parse(bytes, (uint32_t)len, &m) != 0) {
        LOGI("[cuevr] the %s render model would not parse: %s",
             hand ? "right" : "left", cuevr_glb_error());
        return;
    }
    /* The scene's meshes index with 16 bits. A controller is a few thousand
     * triangles so this should never fire, but silently drawing a third of a
     * model is worse than keeping the proxy. */
    if (m.nv > 65535) {
        LOGI("[cuevr] the %s render model has %d vertices; keeping the proxy",
             hand ? "right" : "left", m.nv);
        cuevr_glb_free(&m);
        return;
    }

    rm_free(hand);

    Builder b;
    b_init(&b, m.nv, m.ni);
    for (int i = 0; i < m.nv; i++)
        b_vert(&b, m.v[i].p[0], m.v[i].p[1], m.v[i].p[2],
                   m.v[i].n[0], m.v[i].n[1], m.v[i].n[2],
                   m.v[i].uv[0], m.v[i].uv[1]);
    for (int i = 0; i < m.ni; i++) b.i[b.ni++] = (uint16_t)m.idx[i];
    mesh_upload(&G.rm[hand].mesh, &b);
    b_free(&b);

    for (int i = 0; i < m.ntex; i++) {
        glGenTextures(1, &G.rm[hand].tex[i]);
        glBindTexture(GL_TEXTURE_2D, G.rm[hand].tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m.tex[i].w, m.tex[i].h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, m.tex[i].px);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    G.rm[hand].ntex = m.ntex;

    G.rm[hand].nparts = m.nparts;
    for (int i = 0; i < m.nparts; i++) {
        G.rm[hand].part[i].first = m.part[i].first_index;
        G.rm[hand].part[i].count = m.part[i].n_index;
        G.rm[hand].part[i].tex   = m.part[i].tex;
        memcpy(G.rm[hand].part[i].base, m.part[i].base, sizeof m.part[i].base);
    }
    G.rm[hand].have = 1;

    /* The bounding box, logged. If the model ever comes out mis-oriented on
     * hardware this is the number that says so, and it is the one thing that
     * cannot be recovered afterwards from a screenshot. */
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < m.nv; i++)
        for (int k = 0; k < 3; k++) {
            if (m.v[i].p[k] < lo[k]) lo[k] = m.v[i].p[k];
            if (m.v[i].p[k] > hi[k]) hi[k] = m.v[i].p[k];
        }
    LOGI("[cuevr] %s render model: %d tris, %d parts, %d textures, "
         "bounds x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]",
         hand ? "right" : "left", m.ni / 3, m.nparts, m.ntex,
         lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
    cuevr_glb_free(&m);
}

int cuevr_render_has_ctrl_model(int hand) {
    return (hand >= 0 && hand <= 1) ? G.rm[hand].have : 0;
}

/* ---- the lighting rig ---------------------------------------------------- *
 * Selected here rather than passed in the scene, because it changes once when
 * the player picks it and then not again for the whole frame — and because the
 * rig has to be REBUILT against the table, so it cannot just be a number the
 * draw call reads. */
/* The body under the slate. -1 asks cuevr_frame.c which design suits the table,
 * which is the default and what most players will leave it on. Takes effect on
 * the next set_table, so the menu's live preview re-racks and rebuilds together. */
/* ---- feature toggles ----------------------------------------------------- */
/* Defaults chosen from play, not from the measurements: shadows and the ball
 * reflections earn their cost, the frame body's timber does not — you never see
 * apron grain from playing distance and it was the single most expensive thing
 * drawn. The nap stays off; it is nearly free but was not missed. */
/* Shadows OFF by default: the shadow-map pass costs more frame time on the
 * Quest than it earns, and the menu can switch it on for machines with the
 * headroom. */
static int s_fx[CUEVR_FX_N] = { 0, 1, 1, 0, 0 };
static const char *FX_NAME[CUEVR_FX_N] = {
    "SHADOWS", "VARNISH", "BALL REFLECT", "CLOTH NAP", "FRAME WOOD" };
void cuevr_render_fx_set(int w, int on) { if (w >= 0 && w < CUEVR_FX_N) s_fx[w] = on ? 1 : 0; }
int  cuevr_render_fx(int w) { return (w >= 0 && w < CUEVR_FX_N) ? s_fx[w] : 1; }
const char *cuevr_render_fx_name(int w) {
    return (w >= 0 && w < CUEVR_FX_N) ? FX_NAME[w] : "?";
}

void cuevr_render_set_body(int i) {
    G.frame_sel = (i >= 0 && i < CUEVR_FRAME_COUNT) ? i : -1;
}
int cuevr_render_body(void) { return G.frame_sel; }
int cuevr_render_body_count(void) { return CUEVR_FRAME_COUNT; }
const char *cuevr_render_body_name(int i) {
    return (i >= 0 && i < CUEVR_FRAME_COUNT) ? CUEVR_FRAMES[i].name : "AUTO";
}

int cuevr_render_light_count(void) { return CUEVR_LIGHT_N; }
const char *cuevr_render_light_name(int i) { return cuevr_light_name(i); }
int cuevr_render_light(void) { return G.light_mode; }
void cuevr_render_set_light(int i) {
    if (i < 0 || i >= CUEVR_LIGHT_N) i = 0;
    G.light_mode = i;
    cuevr_light_build(i, &G.tab, &G.rig);
}
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
    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    if (!vs) return -1;
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    GLint ok = 0;
    if (fs) {
        G.prog = glCreateProgram();
        glAttachShader(G.prog, vs);
        glAttachShader(G.prog, fs);
        glLinkProgram(G.prog);
        glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char l[4096];
            glGetProgramInfoLog(G.prog, sizeof l, NULL, l);
            LOGI("[cuevr] link: %s", l);
            glDeleteProgram(G.prog);
            G.prog = 0;
        }
        glDeleteShader(fs);
    }
    if (!ok) {
        /* Draw SOMETHING. See FS_MIN. */
        LOGI("[cuevr] the shading program failed — falling back to flat shading");
        GLuint fm = compile(GL_FRAGMENT_SHADER, FS_MIN);
        if (!fm) { glDeleteShader(vs); return -1; }
        G.prog = glCreateProgram();
        glAttachShader(G.prog, vs);
        glAttachShader(G.prog, fm);
        glLinkProgram(G.prog);
        glGetProgramiv(G.prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char l[4096];
            glGetProgramInfoLog(G.prog, sizeof l, NULL, l);
            LOGI("[cuevr] even the fallback would not link: %s", l);
            glDeleteShader(vs); glDeleteShader(fm);
            return -1;
        }
        glDeleteShader(fm);
        G.minimal = 1;
    }
    glDeleteShader(vs);

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
    G.u_lampG      = glGetUniformLocation(G.prog, "u_lampG[0]");
    G.u_lampN      = glGetUniformLocation(G.prog, "u_lampN[0]");
    G.u_lampI      = glGetUniformLocation(G.prog, "u_lampI[0]");
    G.u_nlamp      = glGetUniformLocation(G.prog, "u_nlamp");
    G.u_lampround  = glGetUniformLocation(G.prog, "u_lampround");
    G.u_keyc       = glGetUniformLocation(G.prog, "u_keyc");
    G.u_fill       = glGetUniformLocation(G.prog, "u_fill");
    G.u_hudv       = glGetUniformLocation(G.prog, "u_hudv");
    G.u_shadow     = glGetUniformLocation(G.prog, "u_shadow");
    G.u_clothlod   = glGetUniformLocation(G.prog, "u_clothlod");
    G.u_rawcol     = glGetUniformLocation(G.prog, "u_rawcol");
    G.u_varn       = glGetUniformLocation(G.prog, "u_varn");
    G.u_shmap      = glGetUniformLocation(G.prog, "u_shmap");
    G.u_shmat      = glGetUniformLocation(G.prog, "u_shmat");
    G.u_shon       = glGetUniformLocation(G.prog, "u_shon");
    G.u_shtexel    = glGetUniformLocation(G.prog, "u_shtexel");
    G.u_shsoft     = glGetUniformLocation(G.prog, "u_shsoft");
    G.u_norefl     = glGetUniformLocation(G.prog, "u_norefl");
    G.u_eye        = glGetUniformLocation(G.prog, "u_eye");
    G.u_clothsh    = glGetUniformLocation(G.prog, "u_clothsh");
    G.u_cloth      = glGetUniformLocation(G.prog, "u_cloth");
    G.u_fur        = glGetUniformLocation(G.prog, "u_fur");
    G.u_nap        = glGetUniformLocation(G.prog, "u_nap");
    G.u_feltspan   = glGetUniformLocation(G.prog, "u_feltspan");
    G.u_half       = glGetUniformLocation(G.prog, "u_half");
    G.u_furslice   = glGetUniformLocation(G.prog, "u_furslice");
    G.u_furslices  = glGetUniformLocation(G.prog, "u_furslices");
    G.u_furdbg     = glGetUniformLocation(G.prog, "u_furdbg");
    G.u_cshaft     = glGetUniformLocation(G.prog, "u_cshaft");
    G.u_csplice    = glGetUniformLocation(G.prog, "u_csplice");
    G.u_cvnr2      = glGetUniformLocation(G.prog, "u_cvnr2");
    G.u_cwrapc     = glGetUniformLocation(G.prog, "u_cwrapc");
    G.u_csleevec   = glGetUniformLocation(G.prog, "u_csleevec");
    G.u_cringc     = glGetUniformLocation(G.prog, "u_cringc");
    G.u_cpts       = glGetUniformLocation(G.prog, "u_cpts");
    G.u_cptlen     = glGetUniformLocation(G.prog, "u_cptlen");
    G.u_cnvnr      = glGetUniformLocation(G.prog, "u_cnvnr");
    G.u_clam       = glGetUniformLocation(G.prog, "u_clam");
    G.u_cvw        = glGetUniformLocation(G.prog, "u_cvw");
    G.u_cv2on      = glGetUniformLocation(G.prog, "u_cv2on");
    G.u_cdiac      = glGetUniformLocation(G.prog, "u_cdiac");
    G.u_cwrap      = glGetUniformLocation(G.prog, "u_cwrap");
    G.u_csleeve    = glGetUniformLocation(G.prog, "u_csleeve");
    G.u_cdia       = glGetUniformLocation(G.prog, "u_cdia");
    G.u_cvcol      = glGetUniformLocation(G.prog, "u_cvcol");
    G.u_cnvcol     = glGetUniformLocation(G.prog, "u_cnvcol");
    G.u_csfig      = glGetUniformLocation(G.prog, "u_csfig");
    G.u_cbfig      = glGetUniformLocation(G.prog, "u_cbfig");
    G.u_cishape    = glGetUniformLocation(G.prog, "u_cishape");
    G.u_cipearl    = glGetUniformLocation(G.prog, "u_cipearl");
    G.u_cit        = glGetUniformLocation(G.prog, "u_cit");
    G.u_chand      = glGetUniformLocation(G.prog, "u_chand");
    G.u_cnarch     = glGetUniformLocation(G.prog, "u_cnarch");
    G.u_cpflip     = glGetUniformLocation(G.prog, "u_cpflip");
    G.u_cppearl    = glGetUniformLocation(G.prog, "u_cppearl");
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
    /* Before anything can draw. The rig carries the light's COLOUR, which
     * multiplies every fragment — so a zeroed rig is not "no lighting chosen",
     * it is a black screen, and set_table (which builds it) has early returns. */
    cuevr_light_build(G.light_mode, t, &G.rig);

    cuevr_render_set_table(t, w);

    Builder b;
    b_init(&b, 4096, 12288);
    build_sphere(&b, 24, 16);
    mesh_upload(&G.ball, &b);
    b_free(&b);

    b_init(&b, 20 * 64 + 4096, 20 * 64 * 6 + 4096);   /* + the end cap */
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

    /* A 1x1 white texture, so a render-model part with no base-colour map can
     * be drawn by exactly the same code as one that has. A branch in a fragment
     * shader to avoid a texture fetch is a poor trade. */
    {
        const uint8_t w[4] = { 255, 255, 255, 255 };
        glGenTextures(1, &s_white);
        glBindTexture(GL_TEXTURE_2D, s_white);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, w);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    /* The shadow map. 1024 over a 12 ft table is a 3.5 mm texel, which with one
     * hardware PCF fetch gives a penumbra about that wide — small and soft,
     * which is what a table lit by wide overhead sources actually has. */
    /* 2048, not 1024. At 1024 the ortho box over a 12 ft table gives a 4.6 mm
     * texel — a ball's shadow is eleven texels across and reads as a jagged
     * stencil. Halving that, with four PCF taps over it, is what makes it soft. */
    G.sh_size = 2048;
    {
        GLuint vs2 = compile(GL_VERTEX_SHADER, VS_SHADOW);
        GLuint fs2 = compile(GL_FRAGMENT_SHADER, FS_SHADOW);
        if (vs2 && fs2) {
            G.sh_prog = glCreateProgram();
            glAttachShader(G.sh_prog, vs2);
            glAttachShader(G.sh_prog, fs2);
            glLinkProgram(G.sh_prog);
            GLint ok2 = 0;
            glGetProgramiv(G.sh_prog, GL_LINK_STATUS, &ok2);
            if (!ok2) { glDeleteProgram(G.sh_prog); G.sh_prog = 0; }
            else {
                G.sh_u_lightvp = glGetUniformLocation(G.sh_prog, "u_lightvp");
                G.sh_u_model   = glGetUniformLocation(G.sh_prog, "u_model");
            }
        }
        if (vs2) glDeleteShader(vs2);
        if (fs2) glDeleteShader(fs2);
    }
    if (G.sh_prog) {
        glGenTextures(1, &G.sh_tex);
        glBindTexture(GL_TEXTURE_2D, G.sh_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, G.sh_size, G.sh_size,
                     0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        /* Comparison sampling: the fetch returns "is this lit", filtered in
         * hardware across four texels for the price of one. */
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glGenFramebuffers(1, &G.sh_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, G.sh_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               G.sh_tex, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGI("[cuevr] no shadow map: incomplete framebuffer");
            glDeleteFramebuffers(1, &G.sh_fbo); G.sh_fbo = 0;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

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
    /* No pocket cones here: the tray under the frame is the floor of every
     * pocket now, and a cone inside it is a second, shallower one. */
    cue_render_pocket_voids(0);
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

    /* The rig is fitted to the table too: the shades hang over THIS table's
     * length, so a 7 ft pub table does not get a 12 ft match table's bar. */
    cuevr_light_build(G.light_mode, t, &G.rig);

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
            /* The body is made of the same timber as the rails. t->rail is the
             * authored RGB565 the FRAME menu row chose. */
            float tw[3] = { ((t->rail >> 11) & 31) / 31.0f,
                            ((t->rail >>  5) & 63) / 63.0f,
                            ( t->rail        & 31) / 31.0f };
            cuevr_frame_set_timber(tw);
            int fs = G.frame_sel < 0 ? cuevr_frame_default(t) : G.frame_sel;
            cuevr_frame_build(fs, &fm, t, w);
            if (fm.overflow) LOGI("[cuevr] frame '%s' ran out of room",
                                  CUEVR_FRAMES[fs].name);
            /* CueVrFrameVtx and the renderer's Vtx are the same layout, so this
             * goes straight to the GPU. */
            Builder fb;
            fb.v = (Vtx *)fm.v; fb.i = fm.idx;
            fb.nv = fm.nv; fb.ni = fm.ni; fb.cap_v = cv; fb.cap_i = ci;
            mesh_upload(&G.frame, &fb);
            G.frame_timber_n = fm.n_timber_idx;
            LOGI("[cuevr] frame '%s': %d tris (%d timber, %d other)",
                 CUEVR_FRAMES[fs].name, fm.ni / 3,
                 fm.n_timber_idx / 3, (fm.ni - fm.n_timber_idx) / 3);
        }
        free(fm.v); free(fm.idx);
    }

    LOGI("[cuevr] table mesh %d tris (%d bed, %d lip), balls re-baked (%s)",
         n, bed, nlip, t->is_snooker ? "snooker" : "pool");
}

void cuevr_render_shutdown(void) {
    if (!G.ready) return;
    mesh_free(&G.ctrl[0]); mesh_free(&G.ctrl[1]);
    rm_free(0); rm_free(1);
    if (s_white) { glDeleteTextures(1, &s_white); s_white = 0; }
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
    /* The host owns the framebuffer and the viewport; the shadow pass borrows
     * both and must hand them back exactly as they were. */
    GLint fbo_before = 0, vp_before[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_before);
    glGetIntegerv(GL_VIEWPORT, vp_before);
    for (int v = 0; v < VP_n; v++) mm4_mul(VP[v], proj + v * 16, view + v * 16);

    /* ---- the shadow pass ---- *
     * Once a frame, before anything else, and view-independent — multiview does
     * not double it. The casters are the balls and the cue: the frame and the
     * rails cast onto nothing you can see, and leaving them out keeps the pass
     * to a couple of thousand triangles of depth-only work. */
    float SHMAT[16];
    int sh_ready = 0;
    /* OFF BY DEFAULT. The map is correct — the cue casts, balls shade each other
     * in the pack, a ball frozen on a cushion is shaded by it — and it still
     * looks worse than the decals it was meant to replace: the shadows come out
     * weak and the balls read as less grounded, which on a table is the whole
     * job of a shadow. Even at 2048 with four PCF taps, a shadow map is a hard
     * test softened at the edges, and what a ball on cloth under wide overhead
     * lamps actually has is a small soft blob — which is exactly what a decal
     * IS. Kept behind CUEVR_SHMAP=1 rather than deleted, because it is the right
     * answer for the cue's shadow and for the pack, and worth returning to. */
    if (G.sh_fbo && G.sh_prog && s_fx[CUEVR_FX_SHADOWS] && !getenv("CUEVR_NOSHMAP")) {
        MoteVrV3 Ld = G.key_room;
        if (mv3_len(Ld) < 1e-3f) Ld = mv3(0, 1, 0);
        Ld = mv3_norm(Ld);
        /* An orthographic box over the table, looking down the key. Sized from
         * the table's own diagonal plus a margin so a ball frozen on a cushion
         * is still inside the map — a caster that falls outside it is not
         * unshadowed, it is a shadow that vanishes as you move. */
        float ext = sqrtf(G.tab.half_len * G.tab.half_len
                        + G.tab.half_wid * G.tab.half_wid) + 0.35f;
        MoteVrV3 ctr = s->place->pos;
        MoteVrV3 eye = mv3_add(ctr, mv3_scale(Ld, 2.2f));
        MoteVrV3 up  = fabsf(Ld.y) > 0.95f ? mv3(1, 0, 0) : mv3(0, 1, 0);
        MoteVrV3 zx  = mv3_norm(mv3_sub(eye, ctr));
        MoteVrV3 xx  = mv3_norm(mv3_cross(up, zx));
        MoteVrV3 yy  = mv3_cross(zx, xx);
        float V[16] = { xx.x, yy.x, zx.x, 0,
                        xx.y, yy.y, zx.y, 0,
                        xx.z, yy.z, zx.z, 0,
                        -(xx.x*eye.x + xx.y*eye.y + xx.z*eye.z),
                        -(yy.x*eye.x + yy.y*eye.y + yy.z*eye.z),
                        -(zx.x*eye.x + zx.y*eye.y + zx.z*eye.z), 1 };
        float n0 = 0.05f, f0 = 5.0f;
        float P[16] = { 1.0f/ext, 0, 0, 0,
                        0, 1.0f/ext, 0, 0,
                        0, 0, -2.0f/(f0-n0), 0,
                        0, 0, -(f0+n0)/(f0-n0), 1 };
        mm4_mul(SHMAT, P, V);

        glBindFramebuffer(GL_FRAMEBUFFER, G.sh_fbo);
        glViewport(0, 0, G.sh_size, G.sh_size);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        glUseProgram(G.sh_prog);
        glUniformMatrix4fv(G.sh_u_lightvp, 1, GL_FALSE, SHMAT);
        {   /* the balls */
            float T2[16];
            MoteVrPose bp;
            bp.q = mq_axis_angle(mv3(0, 1, 0), s->place->yaw);
            bp.p = s->place->pos;
            float TT[16];
            mm4_from_pose(TT, bp, 1.0f);
            for (int i = 0; i < s->nballs; i++) {
                const CueBall *bl = &s->balls[i];
                if (!bl->on) continue;
                float L2[16];
                mm4_identity(L2);
                L2[0] = L2[5] = L2[10] = G.tab.R;
                L2[12] = bl->pos.x; L2[13] = bl->pos.y; L2[14] = bl->pos.z;
                mm4_mul(T2, TT, L2);
                glUniformMatrix4fv(G.sh_u_model, 1, GL_FALSE, T2);
                glBindVertexArray(G.ball.vao);
                glDrawElements(GL_TRIANGLES, G.ball.n, GL_UNSIGNED_SHORT, 0);
            }
        }
        if (s->cue_visible && G.cue.n && !getenv("CUEVR_NOCUE")) {
            MoteVrV3 d = mv3_sub(s->cue_butt, s->cue_tip);
            float len = mv3_len(d);
            if (len > 0.02f) {
                MoteVrV3 u = mv3_scale(d, 1.0f / len);
                MoteVrV3 up2 = mv3(0, 1, 0);
                MoteVrV3 ax = mv3_cross(up2, u);
                float s_ = mv3_len(ax), c_ = mv3_dot(up2, u);
                MoteVrQ q = (s_ < 1e-5f)
                    ? (c_ > 0.0f ? mq_ident() : mq_axis_angle(mv3(1,0,0), PI))
                    : mq_axis_angle(ax, atan2f(s_, c_));
                MoteVrPose cp; cp.p = s->cue_tip; cp.q = q;
                float M2[16];
                mm4_from_pose(M2, cp, 1.0f);
                glUniformMatrix4fv(G.sh_u_model, 1, GL_FALSE, M2);
                glBindVertexArray(G.cue.vao);
                glDrawElements(GL_TRIANGLES, G.cue.n, GL_UNSIGNED_SHORT, 0);
            }
        }
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_before);
        glViewport(vp_before[0], vp_before[1], vp_before[2], vp_before[3]);
        sh_ready = 1;
    }

    glUseProgram(G.prog);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    if (sh_ready) {
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, G.sh_tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(G.u_shmap, 5);
        glUniformMatrix4fv(G.u_shmat, 1, GL_FALSE, SHMAT);
        glUniform1f(G.u_shtexel, (float)G.sh_size);
        { const char *v = getenv("CUEVR_SHSOFT");
          glUniform1f(G.u_shsoft, v ? (float)atof(v) : 3.0f); }
    }
    glUniform1f(G.u_shon, sh_ready ? 1.0f : 0.0f);
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
    /* Which rig, and where its fixtures end up in the room. The layout comes
     * from cuevr_light.c — sizes, heights and counts that are the real ones —
     * and all that happens here is the rotation out of table space into the
     * room, which is the same yaw the table itself is carried out by. Vectors
     * rotate; only the centre translates. */
    float cy = cosf(s->place->yaw), sy = sinf(s->place->yaw);
    const CueVrLightRig *rig = &G.rig;
    int nlamp = rig->nlamp;
    {
        float cen[CUEVR_MAX_LAMPS*3], axx[CUEVR_MAX_LAMPS*3], axz[CUEVR_MAX_LAMPS*3];
        float gain[CUEVR_MAX_LAMPS];
        for (int i = 0; i < nlamp; i++) {
            const CueVrLamp *l = &rig->lamp[i];
            /* THE SAME ROTATION THE TABLE USES. cuevr_table_to_room is
             *     x' = x*cos + z*sin,  z' = -x*sin + z*cos
             * and this was the TRANSPOSE of it — the opposite rotation. The
             * table is always sited at a yaw taken from the player's head
             * direction, so the lamps have been hanging mirrored about the
             * table's own axis: the reflections tracked the head correctly and
             * were reflecting lights that were in the wrong place, which is
             * exactly what "they move, but not how lights above the table
             * should" looks like. */
            cen[i*3+0] = s->place->pos.x + l->c[0] * cy + l->c[2] * sy;
            cen[i*3+1] = s->place->pos.y + l->c[1];
            cen[i*3+2] = s->place->pos.z - l->c[0] * sy + l->c[2] * cy;
            axx[i*3+0] = l->ax[0] * cy + l->ax[2] * sy;
            axx[i*3+1] = l->ax[1];
            axx[i*3+2] = -l->ax[0] * sy + l->ax[2] * cy;
            axz[i*3+0] = l->az[0] * cy + l->az[2] * sy;
            axz[i*3+1] = l->az[1];
            axz[i*3+2] = -l->az[0] * sy + l->az[2] * cy;
            gain[i] = l->gain;
        }
        glUniform3fv(G.u_lampC, nlamp, cen);
        glUniform3fv(G.u_lampX, nlamp, axx);
        glUniform3fv(G.u_lampZ, nlamp, axz);
        glUniform1fv(G.u_lampG, nlamp, gain);
        {   /* what the fragment shader used to work out per pixel */
            float pn[CUEVR_MAX_LAMPS*3], iv[CUEVR_MAX_LAMPS*2];
            for (int i = 0; i < nlamp; i++) {
                const float *X = &axx[i*3], *Z = &axz[i*3];
                pn[i*3+0] = X[1]*Z[2] - X[2]*Z[1];
                pn[i*3+1] = X[2]*Z[0] - X[0]*Z[2];
                pn[i*3+2] = X[0]*Z[1] - X[1]*Z[0];
                float xx = X[0]*X[0] + X[1]*X[1] + X[2]*X[2];
                float zz = Z[0]*Z[0] + Z[1]*Z[1] + Z[2]*Z[2];
                iv[i*2+0] = xx > 1e-9f ? 1.0f / xx : 0.0f;
                iv[i*2+1] = zz > 1e-9f ? 1.0f / zz : 0.0f;
            }
            glUniform3fv(G.u_lampN, nlamp, pn);
            glUniform2fv(G.u_lampI, nlamp, iv);
        }
        glUniform1i(G.u_nlamp, nlamp);
        glUniform1f(G.u_lampround, rig->round ? 1.0f : 0.0f);
        glUniform1f(G.u_fill, rig->fill);
        glUniform3fv(G.u_keyc, 1, rig->keyc);
        glUniform2f(G.u_shadow, rig->soft, rig->dark);
        /* Plain by default. The nap was the most expensive thing on screen and
         * very little of it survived at playing distance. CUEVR_NAPCLOTH brings
         * it back for comparison. */
        glUniform1f(G.u_clothlod, (s_fx[CUEVR_FX_NAP] || getenv("CUEVR_NAPCLOTH")) ? 1.0f : 0.0f);
        { const char *rc = getenv("CUEVR_RAWCOL");
          glUniform1f(G.u_rawcol, rc ? (float)atof(rc) : 0.0f); }
        glUniform1f(G.u_norefl, (!s_fx[CUEVR_FX_REFLECT] || getenv("CUEVR_NOREFL")) ? 1.0f : 0.0f);
        /* x = the Kajiya-Kay exponent (how tight the sheen is along the grain),
         * y = spare, z = strength. Left as knobs so the finish can be tuned on
         * the headset without a rebuild. */
        { float ax = 34.0f, ay = 0.25f, k = 0.22f;
          const char *v;
          if ((v = getenv("CUEVR_VAX"))) ax = (float)atof(v);
          if ((v = getenv("CUEVR_VAY"))) ay = (float)atof(v);
          if ((v = getenv("CUEVR_VK")))  k  = (float)atof(v);
          if (!s_fx[CUEVR_FX_VARNISH]) k = 0.0f;
          glUniform3f(G.u_varn, ax, ay, k); }
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

    /* The key, rotated with the table so it is over the cloth and not over your
     * kitchen. The match rig's direction is the handheld's, unchanged. */
    {
        /* Same rotation as the lamps and as cuevr_table_to_room. */
        MoteVrV3 k = mv3_norm(mv3(rig->key[0] * cy + rig->key[2] * sy, rig->key[1],
                                  -rig->key[0] * sy + rig->key[2] * cy));
        glUniform3f(G.u_light, k.x, k.y, k.z);
        G.key_room = k;
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
    if (G.frame.n && !getenv("CUEVR_NOFRAME")) {
        int fx_wood = s_fx[CUEVR_FX_FRAME];
        set_model(T);
        glBindVertexArray(G.frame.vao);
        if (G.frame_timber_n > 0) {
            glUniform1i(G.u_mode, fx_wood ? 7 : 12);   /* 12 = flat, no timber */
            glDrawElements(GL_TRIANGLES, G.frame_timber_n, GL_UNSIGNED_SHORT,
                           (void *)0);
        }
        if (G.frame.n > G.frame_timber_n) {
            glUniform1i(G.u_mode, 12);         /* brass, chrome, laminate, void */
            glDrawElements(GL_TRIANGLES, G.frame.n - G.frame_timber_n,
                           GL_UNSIGNED_SHORT,
                           (void *)(intptr_t)(G.frame_timber_n * 2));
        }
        glBindVertexArray(0);
    }

    /* No backface culling on the table. cue_render's mesh is authored for a
     * software rasteriser that does not cull — the cloth fan, the cushion
     * faces and the pocket voids are wound for shading, not for a front-face
     * convention — so culling it silently drops the bed and half the rails.
     * Depth sorts it correctly regardless. */
    glDisable(GL_CULL_FACE);
    glUniform1i(G.u_mode, 4);          /* vertex colours, as authored */
    if (getenv("CUEVR_NOTABLE")) goto after_table;
    set_model(T);
    draw(&G.table);
after_table: ;

    if (getenv("CUEVR_NOSHADOW")) goto skip_shadows;
    /* ---- the cloth, with its chalk ---- *
     * The spots come from the table itself rather than from anything invented
     * here: snooker's six colours sit on the D ends, the centre of the D, and
     * the blue, pink and black spots along the middle; a pool table keeps its
     * centre and black spots. Same numbers the rack is laid out from, so the
     * chalk is under the ball rather than near it. */
    if (G.bed.n && !getenv("CUEVR_NOTABLE")) {
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
            /* A pool table's two spots are the FOOT spot the rack apex sits on
             * and the HEAD spot on the string. Not the centre of the table and
             * not black_x, which is a snooker field and zero here — so both
             * chalk marks were being drawn on top of each other in the middle
             * of the bed. */
            sp[ns*2] = tb->half_len * 0.5f; sp[ns*2+1] = 0.0f;  ns++;   /* foot */
            if (tb->baulk_x != 0.0f) {
                sp[ns*2] = tb->baulk_x; sp[ns*2+1] = 0.0f;      ns++;   /* head */
            }
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
        glUniform2f(G.u_half, G.tab.half_len, G.tab.half_wid);
        glUniform1f(G.u_furdbg, getenv("CUEVR_FURDBG") ? 1.0f : 0.0f);
        {   const CueVrCueDesign *cd = &CUE_RACK[s_cue_sel];
            glUniform3fv(G.u_cshaft, 1, cd->shaft);
            glUniform3fv(G.u_csplice, 1, cd->splice);
            glUniform3fv(G.u_caccent, 1, cd->accent);
            glUniform3fv(G.u_cbutt, 1, cd->butt);
            glUniform3fv(G.u_cburr, 1, cd->burr);
            glUniform1f(G.u_cflash, (float)cd->flash);
            glUniform3fv(G.u_cvnr2, 1, cd->vnr2);
            glUniform1f(G.u_cv2on, (float)cd->flash2);
            glUniform1f(G.u_cvw, cd->veneer_w > 0.0f ? cd->veneer_w : 0.0010f);
            glUniform3fv(G.u_cwrapc, 1, cd->wrapc);
            glUniform3fv(G.u_csleevec, 1, cd->sleevec);
            glUniform3fv(G.u_cringc, 1, cd->ringc);
            glUniform3fv(G.u_cdiac, 1, cd->diac);
            glUniform1f(G.u_cwrap,  (float)cd->wrap);
            glUniform1f(G.u_csleeve,(float)cd->sleeve);
            glUniform1f(G.u_cdia,   (float)cd->diamonds);
            {   float vflat[18];
                for (int vk = 0; vk < 6; vk++)
                    for (int ck = 0; ck < 3; ck++) vflat[vk*3+ck] = cd->vcol[vk][ck];
                glUniform3fv(G.u_cvcol, 6, vflat); }
            glUniform1f(G.u_cnvcol,  (float)cd->nvcol);
            glUniform1f(G.u_csfig,   (float)cd->shaft_fig);
            glUniform1f(G.u_cbfig,   (float)cd->butt_fig);
            glUniform1f(G.u_cishape, (float)cd->inlay_shape);
            glUniform1f(G.u_cipearl, (float)cd->inlay_pearl);
            glUniform1f(G.u_cit,     cd->inlay_t);
            glUniform1f(G.u_chand,  (float)cd->hand);
            glUniform1f(G.u_cnarch, (float)(cd->arches ? cd->arches : 1));   /* -1 = no panel */
            glUniform1f(G.u_cpflip,  (float)cd->panel_flip);
            glUniform1f(G.u_cppearl, (float)cd->panel_pearl);
            glUniform1f(G.u_cpts,   cd->points   ? (float)cd->points : 4.0f);
            glUniform1f(G.u_cptlen, cd->point_len > 0.01f ? cd->point_len : 1.0f);
            glUniform1f(G.u_cnvnr,  (float)(cd->veneers ? cd->veneers : (cd->flash ? 1 : 0)));
            glUniform1f(G.u_clam,   (float)cd->laminated);
        }
        glUniform1f(G.u_baulk, tb->baulk_x);
        glUniform1f(G.u_drad,  tb->d_radius);
        /* HALF-widths, both of them — which the old comments did not say, so the
         * line was drawn at twice its stated size. A baulk line struck in chalk
         * is about 2 mm across and a spot about 7 mm. */
        glUniform1f(G.u_linew, 0.0010f);      /* 2 mm across */
        glUniform1f(G.u_spotr, 0.0035f);      /* 7 mm across */
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
        /* ONE shadow per ball, small and soft, tucked under it.
         *
         * It briefly became one blob PER FIXTURE, fanned out and faded — on the
         * reasoning that six lights throw six shadows. They do, but a cue sports
         * rig is wide soft sources close overhead, so those six overlap into a
         * single small penumbra directly under the ball. Drawn separately they
         * read as a ring of circles round each ball, which is nothing like a
         * table and is worse than what it replaced.
         *
         * A real one is barely wider than the ball, dark in the middle and gone
         * within a few millimetres. Size, edge and darkness are all overridable
         * so variants can be rendered side by side and chosen. */
        /* The decals are a STAND-IN for the shadow map, not a companion to it.
         * With the map running they are drawn over real shadows and double them,
         * and they cannot do any of the things the map can — the cue's shadow on
         * the cloth, one ball shading another in the pack, a ball shaded against
         * the cushion it is frozen on. */
        float shrad = 1.00f, shsoft = 0.45f, shdark = 0.55f;   /* option A */
        if (sh_ready) shrad = 0.0f;
        { const char *v;
          if ((v = getenv("CUEVR_SH_RAD")))  shrad  = (float)atof(v);
          if ((v = getenv("CUEVR_SH_SOFT"))) shsoft = (float)atof(v);
          if ((v = getenv("CUEVR_SH_DARK"))) shdark = (float)atof(v); }
        glUniform2f(G.u_shadow, shsoft, shdark);
        float rad = G.tab.R * shrad;
        if (getenv("CUEVR_NOBLOB")) rad = 0.0f;
        for (int i = 0; i < s->nballs && rad > 0.0f; i++) {
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
            glUniform4f(G.u_colour, 0.0f, 0.0f, 0.0f, 1.0f);
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
        if (!getenv("CUEVR_NOLIPS")) {
            /* The drop lip is a moulded edge, not a plank: giving it the rails'
             * grain put wood figure on the one part of the pocket that is meant
             * to read as a plain rolled-over edge. Flat vertex colour. */
            glUniform1i(G.u_mode, 12);
            set_model(T);
            draw(&G.lips);
        }
        glDepthMask(GL_TRUE);
    }
    glEnable(GL_CULL_FACE);

    /* ---- the cue ---- */
    /* CUEVR_NOCUE: leave the cue out — a top-down capture wants the cloth
     * markings, and the cue lies straight across the D. */
    if (s->cue_visible && !getenv("CUEVR_NOCUE")) {
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
            /* Roll first, about the mesh's own +Y axis, THEN swing that axis
             * onto the cue line — the other order would roll about the world's
             * vertical and tumble the cue instead of spinning it. */
            if (s->cue_roll != 0.0f)
                q = mq_mul(q, mq_axis_angle(mv3(0, 1, 0), s->cue_roll));
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
            if (G.rm[i].have) {
                /* The runtime's model needs NO model-to-grip matrix: it is
                 * authored in the grip frame, which is the reason for preferring
                 * it over anything shipped in the APK. Straight at the pose. */
                set_model(P);
                glUniform1i(G.u_mode, 11);
                glActiveTexture(GL_TEXTURE0);
                glBindVertexArray(G.rm[i].mesh.vao);
                for (int q = 0; q < G.rm[i].nparts; q++) {
                    int ti = G.rm[i].part[q].tex;
                    glBindTexture(GL_TEXTURE_2D,
                                  (ti >= 0 && ti < G.rm[i].ntex) ? G.rm[i].tex[ti]
                                                                 : s_white);
                    const float *bc = G.rm[i].part[q].base;
                    glUniform4f(G.u_colour, bc[0], bc[1], bc[2], bc[3]);
                    glDrawElements(GL_TRIANGLES, G.rm[i].part[q].count,
                                   GL_UNSIGNED_SHORT,
                                   (void *)(intptr_t)(G.rm[i].part[q].first * 2));
                }
                glBindVertexArray(0);
                glUniform1i(G.u_mode, 0);
                colour(0.16f, 0.17f, 0.19f, 1.0f);
            } else {
                mm4_mul(M, P, K);
                set_model(M);
                draw(G.ctrl[i].n ? &G.ctrl[i] : &G.grip);
            }
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
        /* White light for the scoreboard. It is a lit panel, not a lit SURFACE:
         * a tungsten room should not turn the scores orange any more than it
         * turns a television orange. */
        glUniform3f(G.u_keyc, 1.0f, 1.0f, 1.0f);
        glUniform4f(G.u_colour, 1, 1, 1, 1);
        int rows = (s->hud_rows > 0 && s->hud_rows <= CUEVR_HUD_LH)
                 ? s->hud_rows : CUEVR_HUD_LH;
        float vf = (float)rows / (float)CUEVR_HUD_LH;
        glUniform1f(G.u_hudv, vf);
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
        /* The panel's shape is the shape of the rows in use, not of the whole
         * texture — otherwise a 72-row scoreboard would be drawn on a
         * 112-row-tall board with a third of it empty. */
        S[5] = s->hud_w * (float)rows / (float)CUEVR_HUD_LW;
        float M[16];
        mm4_mul(M, P, S);
        set_model(M);
        draw(&G.quad);
        glEnable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glUniform3fv(G.u_keyc, 1, G.rig.keyc);
        glUniform1f(G.u_hudv, 1.0f);
        glUniform4f(G.u_hudrect, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    glBindVertexArray(0);
}
