/*
 * Mote XR — the one OpenXR bring-up, shared by every headset app here.
 *
 * Everything here is the standard OpenXR bring-up in the order the spec insists
 * on (loader init, instance, system, graphics requirements, session, spaces,
 * swapchains, actions), with three Quest-specific notes worth knowing:
 *
 *  · Passthrough is not a blend mode. Meta's runtime requires the frame's
 *    environmentBlendMode to stay OPAQUE even with passthrough running; the
 *    camera feed arrives as its own composition layer submitted *under* the
 *    projection layer, and what lets it show through is the projection layer
 *    having alpha 0 where nothing was drawn. So the colour buffer is cleared
 *    fully transparent and the layer carries the source-alpha blend flags.
 *    Get that wrong and you get a black void with a console floating in it —
 *    which is exactly what a runtime without XR_FB_passthrough gives you, so
 *    the renderer draws a floor grid there instead of nothing.
 *
 *  · The loader has to be told about the VM before anything else. On Android
 *    xrInitializeLoaderKHR is not optional, and it is fetched through
 *    xrGetInstanceProcAddr with a NULL instance, which is the one place that is
 *    legal.
 *
 *  · The right controller's system button belongs to the system. Only the left
 *    controller has a menu button an app may bind.
 *
 * The frame loop owns no app state at all: it hands the app tracking, then asks
 * it to draw each eye. See MoteXrApp in mote_xr.h.
 */
#include "mote_xr.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#define XR_USE_PLATFORM_ANDROID 1
#include <android/native_window.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_VIEWS 2

#include <android/log.h>
static void xrlog(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "mote", "%s", b);
}

/* ---- state -------------------------------------------------------------- */

typedef struct {
    XrSwapchain handle;
    int32_t     w, h;
    uint32_t    n;
    XrSwapchainImageOpenGLESKHR *images;
    GLuint     *fbo;
    GLuint      depth;      /* renderbuffer (per-eye) or 2-layer texture (multiview) */
} Eye;

/* GL_EXT_multisampled_render_to_texture, resolved at runtime. Declared here
 * rather than pulled from a header so a stale NDK cannot break the build. */
typedef void (*PFN_glFramebufferTexture2DMultisampleEXT)(GLenum, GLenum, GLenum,
                                                         GLuint, GLint, GLsizei);
typedef void (*PFN_glRenderbufferStorageMultisampleEXT)(GLenum, GLsizei, GLenum,
                                                        GLsizei, GLsizei);

/* GL_OVR_multiview2 (+ the multisampled variant), also resolved at runtime.
 *
 * Multiview renders both eyes in ONE pass into a two-layer framebuffer, with
 * gl_ViewID_OVR picking the layer inside the shader. It is the standard way to
 * render on a Quest: the geometry is submitted once instead of twice, so the draw
 * calls and the vertex work halve, and the driver takes a path built for it. */
typedef void (*PFN_glFramebufferTextureMultiviewOVR)(GLenum, GLenum, GLuint,
                                                     GLint, GLint, GLsizei);
typedef void (*PFN_glFramebufferTextureMultisampleMultiviewOVR)(GLenum, GLenum, GLuint,
                                                                GLint, GLsizei, GLint, GLsizei);

static struct {
    XrInstance  instance;
    XrSystemId  system;
    XrSession   session;
    XrSpace     space;          /* LOCAL: the seated origin */
    XrSessionState state;
    int         running;        /* between xrBeginSession and xrEndSession */
    int         quit;

    uint32_t    nview;
    XrViewConfigurationView vcfg[MAX_VIEWS];
    XrView      views[MAX_VIEWS];
    Eye         eye[MAX_VIEWS];

    /* input */
    XrActionSet action_set;
    XrAction    a_pose, a_stick, a_trigger, a_squeeze;
    XrAction    a_lower, a_upper, a_menu, a_haptic;
    XrPath      hand_path[2];
    XrSpace     hand_space[2];

    /* render models — the runtime's own picture of the hardware in your hands.
     * Absent on runtimes without XR_FB_render_model, and absent for a while even
     * on ones that have it: a model does not exist until its controller has been
     * seen, so this is asked for repeatedly rather than once. */
    int   has_render_model;
    int   has_perf;
    int   has_refresh;
    PFN_xrEnumerateRenderModelPathsFB xrEnumerateRenderModelPathsFB_;
    PFN_xrGetRenderModelPropertiesFB  xrGetRenderModelPropertiesFB_;
    PFN_xrLoadRenderModelFB           xrLoadRenderModelFB_;
    int   rm_done[2];        /* stop asking: either handed over, or hopeless */
    int   rm_tries[2];

    /* passthrough (absent on runtimes without XR_FB_passthrough) */
    int                 has_passthrough;
    XrPassthroughFB     passthrough;
    XrPassthroughLayerFB pt_layer;
    PFN_xrCreatePassthroughFB       xrCreatePassthroughFB_;
    PFN_xrDestroyPassthroughFB      xrDestroyPassthroughFB_;
    PFN_xrPassthroughStartFB        xrPassthroughStartFB_;
    PFN_xrCreatePassthroughLayerFB  xrCreatePassthroughLayerFB_;
    PFN_xrPassthroughLayerResumeFB  xrPassthroughLayerResumeFB_;

    /* EGL — ours, not a window system's: OpenXR renders into its own swapchain
     * images, so the only surface needed is a 16x16 pbuffer to make current. */
    EGLDisplay egl_dpy;
    EGLContext egl_ctx;
    EGLSurface egl_surf;
    EGLConfig  egl_cfg;

    MoteXrApp   app;
    int         srgb;
    int         floor;
    XrTime      last_display;
    int msaa;      /* samples per pixel; 0 = off, -1 = not yet decided */
    int multiview; /* 1 = one array swapchain, both eyes in a single pass */
    PFN_glFramebufferTextureMultiviewOVR            p_fbmv;
    PFN_glFramebufferTextureMultisampleMultiviewOVR p_fbmvms;
    PFN_glFramebufferTexture2DMultisampleEXT p_tex2dms;
    PFN_glRenderbufferStorageMultisampleEXT  p_rbms;
} S;

static int failed(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return 0;
    char s[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (S.instance) xrResultToString(S.instance, r, s);
    xrlog("[mote-xr] %s failed: %s (%d)", what, s, (int)r);
    return 1;
}

/* ---- EGL ---------------------------------------------------------------- */

static int egl_up(void) {
    S.egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (S.egl_dpy == EGL_NO_DISPLAY) { xrlog("[mote-xr] no EGL display"); return -1; }
    if (!eglInitialize(S.egl_dpy, NULL, NULL)) { xrlog("[mote-xr] eglInitialize"); return -1; }

    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES, 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(S.egl_dpy, cfg_attr, &S.egl_cfg, 1, &n) || n < 1) {
        xrlog("[mote-xr] no EGL config"); return -1;
    }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    S.egl_ctx = eglCreateContext(S.egl_dpy, S.egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (S.egl_ctx == EGL_NO_CONTEXT) { xrlog("[mote-xr] eglCreateContext"); return -1; }
    const EGLint surf_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    S.egl_surf = eglCreatePbufferSurface(S.egl_dpy, S.egl_cfg, surf_attr);
    if (S.egl_surf == EGL_NO_SURFACE) { xrlog("[mote-xr] eglCreatePbufferSurface"); return -1; }
    if (!eglMakeCurrent(S.egl_dpy, S.egl_surf, S.egl_surf, S.egl_ctx)) {
        xrlog("[mote-xr] eglMakeCurrent"); return -1;
    }
    return 0;
}

/* ---- instance + session -------------------------------------------------- */

static int have_ext(const XrExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

static int make_instance(JavaVM *vm, jobject activity) {
    /* On Android the loader must be initialised before anything else, and the
     * only legal way to reach the function is through a NULL instance. */
    PFN_xrInitializeLoaderKHR init = NULL;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction *)&init);
    if (init) {
        XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
        li.applicationVM = vm;
        li.applicationContext = activity;
        if (failed(init((const XrLoaderInitInfoBaseHeaderKHR *)&li),
                   "xrInitializeLoaderKHR")) return -1;
    }

    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    XrExtensionProperties *ext = calloc(n ? n : 1, sizeof *ext);
    for (uint32_t i = 0; i < n; i++) ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, ext);

    const char *want[8];
    uint32_t nw = 0;
    want[nw++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    want[nw++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    S.has_passthrough = have_ext(ext, n, XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (S.has_passthrough) want[nw++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
    /* Optional, and unlike passthrough its absence costs nothing: the app draws
     * its own controller proxies when there is no model to be had. */
    S.has_render_model = have_ext(ext, n, XR_FB_RENDER_MODEL_EXTENSION_NAME);
    if (S.has_render_model) want[nw++] = XR_FB_RENDER_MODEL_EXTENSION_NAME;
    /* Performance levels. Without this the runtime clocks the SoC for a MENU —
     * it has no way to know it is running a game — and that is the cheapest
     * frame time available anywhere, because it costs no pixels at all. */
    S.has_perf = have_ext(ext, n, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (S.has_perf) want[nw++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
    S.has_refresh = have_ext(ext, n, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (S.has_refresh) want[nw++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    for (uint32_t i = 0; i < nw; i++)
        if (!have_ext(ext, n, want[i])) {
            xrlog("[mote-xr] runtime lacks %s", want[i]);
            free(ext);
            return -1;
        }
    free(ext);
    xrlog("[mote-xr] passthrough %s", S.has_passthrough ? "available" : "NOT available");
    xrlog("[mote-xr] render models %s", S.has_render_model ? "available" : "NOT available");

    XrInstanceCreateInfoAndroidKHR aci = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    aci.applicationVM = vm;
    aci.applicationActivity = activity;

    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.next = &aci;
    ici.enabledExtensionCount = nw;
    ici.enabledExtensionNames = want;
    snprintf(ici.applicationInfo.applicationName,
             sizeof ici.applicationInfo.applicationName, "%s",
             S.app.name ? S.app.name : "Mote");
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName,
             sizeof ici.applicationInfo.engineName, "Mote");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (failed(xrCreateInstance(&ici, &S.instance), "xrCreateInstance")) return -1;

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (failed(xrGetSystem(S.instance, &sgi, &S.system), "xrGetSystem")) return -1;

    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(S.instance, S.system, &sp)))
        xrlog("[mote-xr] %s", sp.systemName);
    return 0;
}

static int make_session(void) {
    /* Required before session creation even though we ignore the answer: the
     * spec makes calling it a precondition, not advice. */
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req = NULL;
    xrGetInstanceProcAddr(S.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction *)&req);
    if (req) {
        XrGraphicsRequirementsOpenGLESKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        req(S.instance, S.system, &gr);
    }

    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    gb.display = S.egl_dpy;
    gb.config  = S.egl_cfg;
    gb.context = S.egl_ctx;

    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = S.system;
    if (failed(xrCreateSession(S.instance, &sci, &S.session), "xrCreateSession")) return -1;

    /* LOCAL for something you hold — it sits where you are and needs no room
     * setup. STAGE when the app says it puts things on the floor, because
     * LOCAL's origin is the HEADSET, so "0.85 m up" in it is 0.85 m above your
     * eyes. STAGE is not guaranteed, so ask the runtime and fall back. */
    XrReferenceSpaceType want = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (S.app.floor_relative) {
        uint32_t ns = 0;
        xrEnumerateReferenceSpaces(S.session, 0, &ns, NULL);
        XrReferenceSpaceType *avail = calloc(ns ? ns : 1, sizeof *avail);
        xrEnumerateReferenceSpaces(S.session, ns, &ns, avail);
        for (uint32_t i = 0; i < ns; i++)
            if (avail[i] == XR_REFERENCE_SPACE_TYPE_STAGE) {
                want = XR_REFERENCE_SPACE_TYPE_STAGE;
                S.floor = 1;
                break;
            }
        free(avail);
        xrlog("[mote-xr] %s space (app wanted the floor)",
              S.floor ? "STAGE" : "LOCAL — no STAGE, falling back");
    }
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = want;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    if (failed(xrCreateReferenceSpace(S.session, &rs, &S.space), "xrCreateReferenceSpace"))
        return -1;
    return 0;
}

static int make_swapchains(void) {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(S.instance, S.system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, NULL);
    if (n > MAX_VIEWS) n = MAX_VIEWS;
    S.nview = n;
    for (uint32_t i = 0; i < n; i++) S.vcfg[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(S.instance, S.system,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, S.vcfg);

    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(S.session, 0, &nf, NULL);
    int64_t *fmt = calloc(nf ? nf : 1, sizeof *fmt);
    xrEnumerateSwapchainFormats(S.session, nf, &nf, fmt);
    /* sRGB where offered: the shaders write linear-ish colour and the panel
     * expects sRGB, and letting the swapchain do the conversion is free. */
    int64_t want = 0;
    for (uint32_t i = 0; i < nf && !want; i++)
        if (fmt[i] == GL_SRGB8_ALPHA8) want = fmt[i];
    for (uint32_t i = 0; i < nf && !want; i++)
        if (fmt[i] == GL_RGBA8) want = fmt[i];
    if (!want && nf) want = fmt[0];
    free(fmt);
    /* Who performs the sRGB encode. Getting this backwards is the washed-out
     * bug, and it is invisible from here — so the app can ask, and it is
     * logged either way. */
    S.srgb = (want == GL_SRGB8_ALPHA8);
    xrlog("[mote-xr] swapchain format 0x%04x (%s)", (unsigned)want,
          S.srgb ? "sRGB, runtime encodes" : "linear, shader encodes");

    /* Multiview, decided before anything is created because it changes the SHAPE
     * of what gets created: one array swapchain instead of two flat ones. It needs
     * the app to supply draw_views as well — a renderer whose shaders are not
     * view-indexed cannot use it, and falling back is not a failure. */
    {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        int have = ext && strstr(ext, "GL_OVR_multiview2");
        S.p_fbmv = have ? (PFN_glFramebufferTextureMultiviewOVR)
            eglGetProcAddress("glFramebufferTextureMultiviewOVR") : NULL;
        S.p_fbmvms = (have && ext && strstr(ext, "GL_OVR_multiview_multisampled_render_to_texture"))
            ? (PFN_glFramebufferTextureMultisampleMultiviewOVR)
              eglGetProcAddress("glFramebufferTextureMultisampleMultiviewOVR") : NULL;
        S.multiview = (S.p_fbmv && S.app.draw_views && n == 2) ? 1 : 0;
        xrlog("[mote-xr] multiview %s%s", S.multiview ? "on" : "off",
              (S.multiview && !S.p_fbmvms) ? " (no multisampled variant: MSAA off)" : "");
    }

    S.msaa = -1;    /* probed once, on the first eye */

    /* Multiview needs ONE swapchain with two layers, not one per eye. */
    for (uint32_t i = 0; i < (S.multiview ? 1u : n); i++) {
        Eye *e = &S.eye[i];
        /* 2x MSAA at a HIGHER resolution rather than 4x at the recommended one.
         *
         * On a tiler the cost of MSAA is tile memory and resolve bandwidth, and
         * it scales with the sample count; the cost of resolution scales with
         * the pixel count. Trading two samples for a quarter more pixels each
         * way is usually cheaper AND sharper, because the extra samples only
         * ever help at edges while the extra pixels help everywhere — text on
         * the scoreboard, the numbers on a ball, the chalk on the cloth.
         *
         * Clamped to the runtime's own maximum: asking for more than
         * maxImageRect is not a request, it is an error. */
        float scale = 1.25f;
        int32_t rw = (int32_t)S.vcfg[i].recommendedImageRectWidth;
        int32_t rh = (int32_t)S.vcfg[i].recommendedImageRectHeight;
        e->w = (int32_t)(rw * scale);
        e->h = (int32_t)(rh * scale);
        if ((uint32_t)e->w > S.vcfg[i].maxImageRectWidth)
            e->w = (int32_t)S.vcfg[i].maxImageRectWidth;
        if ((uint32_t)e->h > S.vcfg[i].maxImageRectHeight)
            e->h = (int32_t)S.vcfg[i].maxImageRectHeight;
        if (i == 0)
            xrlog("[mote-xr] eye %dx%d (recommended %dx%d, max %ux%u)",
                  e->w, e->h, rw, rh,
                  S.vcfg[i].maxImageRectWidth, S.vcfg[i].maxImageRectHeight);

        XrSwapchainCreateInfo sc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = want;
        /* The swapchain itself stays single-sample. MSAA is applied on our own
         * framebuffer with an implicit resolve (see below), which is both the
         * cheap path on tiled hardware and the only one that keeps these images
         * ordinary GL_TEXTURE_2Ds. */
        sc.sampleCount = 1;
        sc.width = e->w;
        sc.height = e->h;
        sc.faceCount = 1;
        sc.arraySize = S.multiview ? 2 : 1;   /* one image, two layers */
        sc.mipCount = 1;
        if (failed(xrCreateSwapchain(S.session, &sc, &e->handle), "xrCreateSwapchain"))
            return -1;

        xrEnumerateSwapchainImages(e->handle, 0, &e->n, NULL);
        e->images = calloc(e->n, sizeof *e->images);
        for (uint32_t k = 0; k < e->n; k++)
            e->images[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(e->handle, e->n, &e->n,
                                   (XrSwapchainImageBaseHeader *)e->images);

        /* MSAA, via GL_EXT_multisampled_render_to_texture.
         *
         * The scene is close to the worst case for aliasing: a handful of smooth
         * curved objects against a real room, where a ball's silhouette and the
         * cue's long taper are exactly the near-vertical edges that crawl as
         * your head moves. There is no geometry density to hide it behind.
         *
         * This extension is the right tool on a tiled GPU. Multisampling happens
         * inside tile memory and resolves on the way out, so the extra samples
         * never cost main-memory bandwidth — which is why it is the standard
         * path on this hardware rather than an explicit multisample texture plus
         * a blit. It also leaves the swapchain images as ordinary textures.
         *
         * Entirely optional: if the extension is missing we render exactly as
         * before. Quality must never be the reason the app fails to start. */
        if (S.msaa < 0) {
            const char *ext = (const char *)glGetString(GL_EXTENSIONS);
            int have = ext && strstr(ext, "GL_EXT_multisampled_render_to_texture");
            S.p_tex2dms = have ? (PFN_glFramebufferTexture2DMultisampleEXT)
                eglGetProcAddress("glFramebufferTexture2DMultisampleEXT") : NULL;
            S.p_rbms = have ? (PFN_glRenderbufferStorageMultisampleEXT)
                eglGetProcAddress("glRenderbufferStorageMultisampleEXT") : NULL;
            if (S.p_tex2dms && S.p_rbms) {
                GLint max_s = 0;
                glGetIntegerv(GL_MAX_SAMPLES, &max_s);
                /* 2x, paired with the resolution increase above. */
                S.msaa = max_s >= 2 ? 2 : 0;
            } else {
                S.msaa = 0;
            }
            xrlog("[mote-xr] MSAA %dx%s", S.msaa ? S.msaa : 1,
                  S.msaa ? "" : " (extension unavailable)");
        }

        if (S.multiview) {
            /* Depth has to be a two-layer TEXTURE, not a renderbuffer: a layered
             * framebuffer's attachments must all be layered. */
            if (S.msaa > 0 && !S.p_fbmvms) S.msaa = 0;
            glGenTextures(1, &e->depth);
            glBindTexture(GL_TEXTURE_2D_ARRAY, e->depth);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH_COMPONENT24, e->w, e->h, 2);
        } else {
            glGenRenderbuffers(1, &e->depth);
            glBindRenderbuffer(GL_RENDERBUFFER, e->depth);
            if (S.msaa > 0)
                S.p_rbms(GL_RENDERBUFFER, S.msaa, GL_DEPTH_COMPONENT24, e->w, e->h);
            else
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e->w, e->h);
        }

        e->fbo = calloc(e->n, sizeof *e->fbo);
        glGenFramebuffers((GLsizei)e->n, e->fbo);
        for (uint32_t k = 0; k < e->n; k++) {
            glBindFramebuffer(GL_FRAMEBUFFER, e->fbo[k]);
            if (S.multiview) {
                if (S.msaa > 0) {
                    S.p_fbmvms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               e->images[k].image, 0, S.msaa, 0, 2);
                    S.p_fbmvms(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                               e->depth, 0, S.msaa, 0, 2);
                } else {
                    S.p_fbmv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             e->images[k].image, 0, 0, 2);
                    S.p_fbmv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                             e->depth, 0, 0, 2);
                }
            } else if (S.msaa > 0) {
                S.p_tex2dms(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            e->images[k].image, 0, S.msaa);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, e->depth);
            } else {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       e->images[k].image, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, e->depth);
            }
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE
                && S.msaa > 0) {
                /* Incomplete with MSAA: drop it and rebuild this one plainly
                 * rather than present a broken eye. */
                xrlog("[mote-xr] MSAA framebuffer incomplete — falling back to 1x");
                S.msaa = 0;
                glBindRenderbuffer(GL_RENDERBUFFER, e->depth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, e->w, e->h);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       e->images[k].image, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, e->depth);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        xrlog("[mote-xr] eye %u: %dx%d, %u images", i, e->w, e->h, e->n);
    }
    return 0;
}

/* ---- input --------------------------------------------------------------- */

static XrAction mk_action(XrActionType type, const char *name, const char *pretty) {
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = type;
    snprintf(ai.actionName, sizeof ai.actionName, "%s", name);
    snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", pretty);
    ai.countSubactionPaths = 2;
    ai.subactionPaths = S.hand_path;
    XrAction a = XR_NULL_HANDLE;
    if (failed(xrCreateAction(S.action_set, &ai, &a), name)) return XR_NULL_HANDLE;
    return a;
}

static XrPath path(const char *s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(S.instance, s, &p);
    return p;
}

static int make_actions(void) {
    XrActionSetCreateInfo asi = { XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(asi.actionSetName, sizeof asi.actionSetName, "mote");
    snprintf(asi.localizedActionSetName, sizeof asi.localizedActionSetName, "Mote");
    if (failed(xrCreateActionSet(S.instance, &asi, &S.action_set), "xrCreateActionSet"))
        return -1;

    S.hand_path[MOTE_VR_LEFT]  = path("/user/hand/left");
    S.hand_path[MOTE_VR_RIGHT] = path("/user/hand/right");

    S.a_pose    = mk_action(XR_ACTION_TYPE_POSE_INPUT,    "grip",    "Hold");
    S.a_stick   = mk_action(XR_ACTION_TYPE_VECTOR2F_INPUT,"stick",   "D-pad");
    S.a_trigger = mk_action(XR_ACTION_TYPE_FLOAT_INPUT,   "trigger", "Shoulder");
    S.a_squeeze = mk_action(XR_ACTION_TYPE_FLOAT_INPUT,   "squeeze", "Resize");
    S.a_lower   = mk_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "lower",   "A");
    S.a_upper   = mk_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "upper",   "B");
    S.a_menu    = mk_action(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu",    "Menu");
    S.a_haptic  = mk_action(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Rumble");
    if (!S.a_pose || !S.a_stick || !S.a_haptic) return -1;

    /* Touch controllers, and the simple profile as a floor so the app is at
     * least usable on a runtime that offers nothing better. */
    XrActionSuggestedBinding tb[] = {
        { S.a_pose,    path("/user/hand/left/input/grip/pose") },
        { S.a_pose,    path("/user/hand/right/input/grip/pose") },
        { S.a_stick,   path("/user/hand/left/input/thumbstick") },
        { S.a_stick,   path("/user/hand/right/input/thumbstick") },
        { S.a_trigger, path("/user/hand/left/input/trigger/value") },
        { S.a_trigger, path("/user/hand/right/input/trigger/value") },
        { S.a_squeeze, path("/user/hand/left/input/squeeze/value") },
        { S.a_squeeze, path("/user/hand/right/input/squeeze/value") },
        { S.a_lower,   path("/user/hand/left/input/x/click") },
        { S.a_lower,   path("/user/hand/right/input/a/click") },
        { S.a_upper,   path("/user/hand/left/input/y/click") },
        { S.a_upper,   path("/user/hand/right/input/b/click") },
        { S.a_menu,    path("/user/hand/left/input/menu/click") },
        { S.a_haptic,  path("/user/hand/left/output/haptic") },
        { S.a_haptic,  path("/user/hand/right/output/haptic") },
    };
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = tb;
    sb.countSuggestedBindings = sizeof tb / sizeof tb[0];
    if (!XR_SUCCEEDED(xrSuggestInteractionProfileBindings(S.instance, &sb)))
        xrlog("[mote-xr] touch bindings rejected");

    XrActionSuggestedBinding kb[] = {
        { S.a_pose,   path("/user/hand/left/input/grip/pose") },
        { S.a_pose,   path("/user/hand/right/input/grip/pose") },
        { S.a_lower,  path("/user/hand/left/input/select/click") },
        { S.a_lower,  path("/user/hand/right/input/select/click") },
        { S.a_menu,   path("/user/hand/left/input/menu/click") },
        { S.a_haptic, path("/user/hand/left/output/haptic") },
        { S.a_haptic, path("/user/hand/right/output/haptic") },
    };
    sb.interactionProfile = path("/interaction_profiles/khr/simple_controller");
    sb.suggestedBindings = kb;
    sb.countSuggestedBindings = sizeof kb / sizeof kb[0];
    xrSuggestInteractionProfileBindings(S.instance, &sb);

    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &S.action_set;
    if (failed(xrAttachSessionActionSets(S.session, &at), "xrAttachSessionActionSets"))
        return -1;

    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo si = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        si.action = S.a_pose;
        si.subactionPath = S.hand_path[i];
        si.poseInActionSpace.orientation.w = 1.0f;
        if (failed(xrCreateActionSpace(S.session, &si, &S.hand_space[i]), "actionSpace"))
            return -1;
    }
    return 0;
}

static void read_hand(int i, MoteVrHand *h, XrTime t) {
    memset(h, 0, sizeof *h);
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.subactionPath = S.hand_path[i];

    gi.action = S.a_stick;
    XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_SUCCEEDED(xrGetActionStateVector2f(S.session, &gi, &v)) && v.isActive) {
        h->stick_x = v.currentState.x;
        h->stick_y = v.currentState.y;
    }
    XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
    gi.action = S.a_trigger;
    if (XR_SUCCEEDED(xrGetActionStateFloat(S.session, &gi, &f)) && f.isActive)
        h->trigger = f.currentState;
    gi.action = S.a_squeeze;
    f.type = XR_TYPE_ACTION_STATE_FLOAT;
    if (XR_SUCCEEDED(xrGetActionStateFloat(S.session, &gi, &f)) && f.isActive)
        h->squeeze = f.currentState;

    XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
    gi.action = S.a_lower;
    if (XR_SUCCEEDED(xrGetActionStateBoolean(S.session, &gi, &b)) && b.isActive)
        h->btn_lower = b.currentState;
    b.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    gi.action = S.a_upper;
    if (XR_SUCCEEDED(xrGetActionStateBoolean(S.session, &gi, &b)) && b.isActive)
        h->btn_upper = b.currentState;
    b.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    gi.action = S.a_menu;
    if (XR_SUCCEEDED(xrGetActionStateBoolean(S.session, &gi, &b)) && b.isActive)
        h->menu = b.currentState;

    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(S.hand_space[i], S.space, t, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        memcpy(&h->pose.q, &loc.pose.orientation, sizeof h->pose.q);
        memcpy(&h->pose.p, &loc.pose.position, sizeof h->pose.p);
        h->tracked = 1;
    }
}

/* Rumble goes to both hands: the console is between them, and a buzz in one
 * hand for an event that belongs to the object as a whole reads as a fault. */
static void haptic(float intensity, int ms) {
    if (!S.session || intensity <= 0.0f || ms <= 0) return;
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = intensity > 1.0f ? 1.0f : intensity;
    v.duration = (XrDuration)ms * 1000000LL;
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    for (int i = 0; i < 2; i++) {
        XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
        hi.action = S.a_haptic;
        hi.subactionPath = S.hand_path[i];
        xrApplyHapticFeedback(S.session, &hi, (const XrHapticBaseHeader *)&v);
    }
}

/* ---- passthrough --------------------------------------------------------- */

static void passthrough_up(void) {
    if (!S.has_passthrough) return;
#define GET(fn) xrGetInstanceProcAddr(S.instance, #fn, (PFN_xrVoidFunction *)&S.fn##_)
    GET(xrCreatePassthroughFB);
    GET(xrDestroyPassthroughFB);
    GET(xrPassthroughStartFB);
    GET(xrCreatePassthroughLayerFB);
    GET(xrPassthroughLayerResumeFB);
#undef GET
    if (!S.xrCreatePassthroughFB_ || !S.xrCreatePassthroughLayerFB_) {
        S.has_passthrough = 0;
        return;
    }
    XrPassthroughCreateInfoFB pci = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
    if (failed(S.xrCreatePassthroughFB_(S.session, &pci, &S.passthrough), "createPassthrough")) {
        S.has_passthrough = 0;
        return;
    }
    XrPassthroughLayerCreateInfoFB lci = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
    lci.passthrough = S.passthrough;
    lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    if (failed(S.xrCreatePassthroughLayerFB_(S.session, &lci, &S.pt_layer), "createPtLayer")) {
        S.has_passthrough = 0;
        return;
    }
    if (S.xrPassthroughStartFB_) S.xrPassthroughStartFB_(S.passthrough);
    if (S.xrPassthroughLayerResumeFB_) S.xrPassthroughLayerResumeFB_(S.pt_layer);
    xrlog("[mote-xr] passthrough running");
}

/* ---- the frame ----------------------------------------------------------- */

static void draw_frame(void) {
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    if (failed(xrWaitFrame(S.session, &fwi, &fs), "xrWaitFrame")) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(S.session, &fbi);

    XrCompositionLayerProjectionView pv[MAX_VIEWS];
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerPassthroughFB ptl = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nlayer = 0;

    if (fs.shouldRender) {
        XrActiveActionSet aas = { S.action_set, XR_NULL_PATH };
        XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
        si.countActiveActionSets = 1;
        si.activeActionSets = &aas;
        xrSyncActions(S.session, &si);

        MoteVrTracking track;
        memset(&track, 0, sizeof track);
        track.dt = S.last_display ? (float)((double)(fs.predictedDisplayTime - S.last_display) * 1e-9)
                                  : 1.0f/72.0f;
        if (track.dt <= 0.0f || track.dt > 0.25f) track.dt = 1.0f/72.0f;
        S.last_display = fs.predictedDisplayTime;

        read_hand(MOTE_VR_LEFT,  &track.hand[MOTE_VR_LEFT],  fs.predictedDisplayTime);
        read_hand(MOTE_VR_RIGHT, &track.hand[MOTE_VR_RIGHT], fs.predictedDisplayTime);

        /* views */
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = S.space;
        uint32_t nv = 0;
        for (uint32_t i = 0; i < S.nview; i++) S.views[i].type = XR_TYPE_VIEW;
        xrLocateViews(S.session, &vli, &vs, S.nview, &nv, S.views);

        if (nv > 0 && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            /* The head is the midpoint of the eyes — good enough, and it is
             * only used to decide which way the console should face. */
            memcpy(&track.head.q, &S.views[0].pose.orientation, sizeof track.head.q);
            track.head.p = mv3_scale(mv3_add(
                mv3(S.views[0].pose.position.x, S.views[0].pose.position.y, S.views[0].pose.position.z),
                mv3(S.views[nv-1].pose.position.x, S.views[nv-1].pose.position.y, S.views[nv-1].pose.position.z)),
                0.5f);
        }

        if (S.app.update) S.app.update(S.app.user, &track);

        if (S.multiview) {
            /* ONE pass, both eyes. The geometry is submitted once into a two-layer
             * framebuffer and gl_ViewID_OVR picks the layer inside the shader, so
             * the draw calls and the vertex work halve. Both projection views then
             * reference the SAME swapchain with different imageArrayIndex — that is
             * the part that is easy to miss and shows up as one eye being black. */
            Eye *e = &S.eye[0];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (!failed(xrAcquireSwapchainImage(e->handle, &ai, &idx), "acquire")) {
                XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(e->handle, &wi);

                glBindFramebuffer(GL_FRAMEBUFFER, e->fbo[idx]);
                glViewport(0, 0, e->w, e->h);
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClearDepthf(1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                float view2[32], proj2[32];
                for (uint32_t i = 0; i < 2; i++) {
                    uint32_t vi = (i < nv) ? i : 0;
                    MoteVrPose ep;
                    memcpy(&ep.q, &S.views[vi].pose.orientation, sizeof ep.q);
                    memcpy(&ep.p, &S.views[vi].pose.position, sizeof ep.p);
                    mm4_view_from_pose(view2 + i * 16, ep);
                    mm4_proj_fov(proj2 + i * 16,
                                 S.views[vi].fov.angleLeft, S.views[vi].fov.angleRight,
                                 S.views[vi].fov.angleUp, S.views[vi].fov.angleDown,
                                 0.02f, 50.0f);
                }
                S.app.draw_views(S.app.user, view2, proj2, !S.has_passthrough);

                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                xrReleaseSwapchainImage(e->handle, &ri);
            }
            for (uint32_t i = 0; i < nv; i++) {
                pv[i] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
                pv[i].pose = S.views[i].pose;
                pv[i].fov  = S.views[i].fov;
                pv[i].subImage.swapchain = e->handle;
                pv[i].subImage.imageArrayIndex = i;      /* the layer for this eye */
                pv[i].subImage.imageRect.offset.x = 0;
                pv[i].subImage.imageRect.offset.y = 0;
                pv[i].subImage.imageRect.extent.width  = e->w;
                pv[i].subImage.imageRect.extent.height = e->h;
            }
        } else
        for (uint32_t i = 0; i < nv; i++) {
            Eye *e = &S.eye[i];
            uint32_t idx = 0;
            XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            if (failed(xrAcquireSwapchainImage(e->handle, &ai, &idx), "acquire")) continue;
            XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(e->handle, &wi);

            glBindFramebuffer(GL_FRAMEBUFFER, e->fbo[idx]);
            glViewport(0, 0, e->w, e->h);
            /* Fully transparent, so passthrough shows wherever nothing is
             * drawn. Without passthrough the renderer fills it with a room. */
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClearDepthf(1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            MoteVrPose eye_pose;
            memcpy(&eye_pose.q, &S.views[i].pose.orientation, sizeof eye_pose.q);
            memcpy(&eye_pose.p, &S.views[i].pose.position, sizeof eye_pose.p);
            float view[16], projm[16];
            mm4_view_from_pose(view, eye_pose);
            mm4_proj_fov(projm, S.views[i].fov.angleLeft, S.views[i].fov.angleRight,
                         S.views[i].fov.angleUp, S.views[i].fov.angleDown, 0.02f, 50.0f);
            if (S.app.draw_eye)
                S.app.draw_eye(S.app.user, view, projm, !S.has_passthrough);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(e->handle, &ri);

            pv[i] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            pv[i].pose = S.views[i].pose;
            pv[i].fov  = S.views[i].fov;
            pv[i].subImage.swapchain = e->handle;
            pv[i].subImage.imageRect.offset.x = 0;
            pv[i].subImage.imageRect.offset.y = 0;
            pv[i].subImage.imageRect.extent.width  = e->w;
            pv[i].subImage.imageRect.extent.height = e->h;
        }

        if (S.has_passthrough) {
            ptl.layerHandle = S.pt_layer;
            ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            ptl.space = XR_NULL_HANDLE;
            layers[nlayer++] = (const XrCompositionLayerBaseHeader *)&ptl;
        }
        proj.space = S.space;
        proj.viewCount = nv;
        proj.views = pv;
        /* The projection layer is composited over the camera feed using the
         * alpha we cleared to, so it must not be treated as opaque. */
        proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        layers[nlayer++] = (const XrCompositionLayerBaseHeader *)&proj;
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    /* Meta's runtime wants OPAQUE even with passthrough: the camera feed is a
     * layer, not a blend mode. */
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = nlayer;
    fei.layers = nlayer ? layers : NULL;
    xrEndFrame(S.session, &fei);
}

static void pump_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(S.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *e =
                (const XrEventDataSessionStateChanged *)&ev;
            S.state = e->state;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (!failed(xrBeginSession(S.session, &bi), "xrBeginSession"))
                    S.running = 1;
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                S.running = 0;
                xrEndSession(S.session);
            } else if (e->state == XR_SESSION_STATE_EXITING ||
                       e->state == XR_SESSION_STATE_LOSS_PENDING) {
                S.quit = 1;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            S.quit = 1;
            break;
        default:
            break;
        }
        ev = (XrEventDataBuffer){ XR_TYPE_EVENT_DATA_BUFFER };
    }
}

/* ---- what the app sees --------------------------------------------------- */

int mote_xr_init(void *vm, void *activity, const MoteXrApp *app) {
    memset(&S, 0, sizeof S);
    if (app) S.app = *app;
    if (egl_up() != 0) return -1;
    if (make_instance((JavaVM *)vm, (jobject)activity) != 0) return -1;
    if (make_session() != 0) return -1;
    if (make_swapchains() != 0) return -1;

    /* 72 Hz, asked for explicitly.
     *
     * The runtime picks a rate on its own and a Quest 3 may offer 72, 80, 90 or
     * 120 — and whatever it picks becomes the deadline every frame has to meet.
     * A cue game wants the LOWEST sensible one: nothing in it moves fast enough
     * to need 90, and 13.9 ms of budget instead of 11.1 is a quarter more time
     * for the same picture. Asking for a rate the headset does not offer is an
     * error rather than a hint, so the list is enumerated first and 72 taken
     * only if it is really there. */
    if (S.has_refresh) {
        PFN_xrEnumerateDisplayRefreshRatesFB enumrr = NULL;
        PFN_xrRequestDisplayRefreshRateFB    setrr  = NULL;
        xrGetInstanceProcAddr(S.instance, "xrEnumerateDisplayRefreshRatesFB",
                              (PFN_xrVoidFunction *)&enumrr);
        xrGetInstanceProcAddr(S.instance, "xrRequestDisplayRefreshRateFB",
                              (PFN_xrVoidFunction *)&setrr);
        if (enumrr && setrr) {
            uint32_t nr = 0;
            enumrr(S.session, 0, &nr, NULL);
            if (nr) {
                float *rates = (float *)calloc(nr, sizeof(float));
                if (rates) {
                    enumrr(S.session, nr, &nr, rates);
                    int have72 = 0;
                    for (uint32_t i = 0; i < nr; i++)
                        if (rates[i] > 71.0f && rates[i] < 73.0f) have72 = 1;
                    if (have72) {
                        XrResult rr = setrr(S.session, 72.0f);
                        xrlog("[mote-xr] display refresh 72 Hz %s",
                              XR_SUCCEEDED(rr) ? "set" : "REFUSED");
                    } else {
                        xrlog("[mote-xr] 72 Hz not offered; leaving the runtime's choice");
                    }
                    free(rates);
                }
            }
        }
    }

    /* SUSTAINED_HIGH on both domains, not BOOST. Boost is for a few seconds of
     * loading; a cue game runs for an hour and being throttled mid-frame is
     * worse than never having had the clocks. */
    if (S.has_perf) {
        PFN_xrPerfSettingsSetPerformanceLevelEXT setlvl = NULL;
        xrGetInstanceProcAddr(S.instance, "xrPerfSettingsSetPerformanceLevelEXT",
                              (PFN_xrVoidFunction *)&setlvl);
        if (setlvl) {
            setlvl(S.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                   XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            setlvl(S.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                   XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
            xrlog("[mote-xr] performance level: sustained high, CPU and GPU");
        }
    }
    if (make_actions() != 0) return -1;
    passthrough_up();
    if (S.app.gl_init && S.app.gl_init(S.app.user) != 0) return -1;
    return 0;
}

int  mote_xr_should_quit(void)     { return S.quit; }
int  mote_xr_running(void)         { return S.running; }
int  mote_xr_has_passthrough(void) { return S.has_passthrough; }
int  mote_xr_target_is_srgb(void)  { return S.srgb; }
int  mote_xr_floor_relative(void)  { return S.floor; }
int  mote_xr_multiview(void)       { return S.multiview; }

/* ---- render models ------------------------------------------------------- *
 *
 * XR_FB_render_model hands over the runtime's own glTF of the controller the
 * player is actually holding, in the controller's own grip space. That last
 * part is the whole value of it: a model shipped with the app has to be aligned
 * to the grip frame by hand, which took nine attempts and was still a guess,
 * and it is the wrong model the moment somebody plays on different hardware.
 *
 * The awkwardness is timing. A model does not exist until its controller has
 * been seen by the runtime, so the first few asks legitimately fail, and asking
 * once at start-up gets you nothing on a headset whose controllers wake on
 * motion. So this is polled: the app calls until it gets something, and after
 * enough refusals it stops so a runtime that will never answer does not cost a
 * path enumeration every frame for the rest of the session.
 *
 * Ownership passes to the caller — it wants to parse the bytes, upload them and
 * forget them, and keeping a megabyte per hand alive here for the life of the
 * app to no purpose would be worse. */
void *mote_xr_render_model_take(int hand, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!S.has_render_model || S.session == XR_NULL_HANDLE) return NULL;
    if (hand < 0 || hand > 1 || S.rm_done[hand]) return NULL;
    if (++S.rm_tries[hand] > 240) {          /* ~4 s of asking at 60 Hz of polls */
        S.rm_done[hand] = 1;
        xrlog("[mote-xr] no render model for the %s controller; using the proxy",
              hand ? "right" : "left");
        return NULL;
    }

    if (!S.xrLoadRenderModelFB_) {
        xrGetInstanceProcAddr(S.instance, "xrEnumerateRenderModelPathsFB",
            (PFN_xrVoidFunction *)&S.xrEnumerateRenderModelPathsFB_);
        xrGetInstanceProcAddr(S.instance, "xrGetRenderModelPropertiesFB",
            (PFN_xrVoidFunction *)&S.xrGetRenderModelPropertiesFB_);
        xrGetInstanceProcAddr(S.instance, "xrLoadRenderModelFB",
            (PFN_xrVoidFunction *)&S.xrLoadRenderModelFB_);
        if (!S.xrLoadRenderModelFB_ || !S.xrGetRenderModelPropertiesFB_) {
            S.rm_done[0] = S.rm_done[1] = 1;
            return NULL;
        }
    }

    /* The path is fixed by the extension, so there is no need to enumerate to
     * find it — but a path that the runtime does not publish is not loadable,
     * and asking for properties on one is how you find that out. */
    XrPath path = XR_NULL_PATH;
    if (xrStringToPath(S.instance,
                       hand ? "/model_fb/controller/right" : "/model_fb/controller/left",
                       &path) != XR_SUCCESS) {
        S.rm_done[hand] = 1;
        return NULL;
    }

    XrRenderModelPropertiesFB props = { XR_TYPE_RENDER_MODEL_PROPERTIES_FB };
    if (S.xrGetRenderModelPropertiesFB_(S.session, path, &props) != XR_SUCCESS)
        return NULL;                            /* not ready yet — ask again */
    if (props.modelKey == XR_NULL_RENDER_MODEL_KEY_FB || props.modelVersion == 0)
        return NULL;

    XrRenderModelLoadInfoFB li = { XR_TYPE_RENDER_MODEL_LOAD_INFO_FB };
    li.modelKey = props.modelKey;
    XrRenderModelBufferFB buf = { XR_TYPE_RENDER_MODEL_BUFFER_FB };
    /* Two calls: the first for the size, the second for the bytes. */
    buf.bufferCapacityInput = 0;
    if (S.xrLoadRenderModelFB_(S.session, &li, &buf) != XR_SUCCESS) return NULL;
    uint32_t n = buf.bufferCountOutput;
    if (!n) return NULL;
    uint8_t *bytes = (uint8_t *)malloc(n);
    if (!bytes) { S.rm_done[hand] = 1; return NULL; }
    buf.bufferCapacityInput = n;
    buf.buffer = bytes;
    if (S.xrLoadRenderModelFB_(S.session, &li, &buf) != XR_SUCCESS) {
        free(bytes);
        return NULL;
    }
    S.rm_done[hand] = 1;
    xrlog("[mote-xr] %s controller render model: %u bytes",
          hand ? "right" : "left", (unsigned)n);
    if (out_len) *out_len = n;
    return bytes;
}
void mote_xr_haptic(float i, int ms) { haptic(i, ms); }

void mote_xr_frame(void) {
    pump_events();
    if (S.running) draw_frame();
}

void mote_xr_shutdown(void) {
    if (S.app.gl_shutdown) S.app.gl_shutdown(S.app.user);
    for (uint32_t i = 0; i < S.nview; i++) {
        Eye *e = &S.eye[i];
        if (e->fbo) { glDeleteFramebuffers((GLsizei)e->n, e->fbo); free(e->fbo); }
        if (e->depth) glDeleteRenderbuffers(1, &e->depth);
        free(e->images);
        if (e->handle) xrDestroySwapchain(e->handle);
    }
    if (S.passthrough && S.xrDestroyPassthroughFB_) S.xrDestroyPassthroughFB_(S.passthrough);
    for (int i = 0; i < 2; i++) if (S.hand_space[i]) xrDestroySpace(S.hand_space[i]);
    if (S.action_set) xrDestroyActionSet(S.action_set);
    if (S.space)    xrDestroySpace(S.space);
    if (S.session)  xrDestroySession(S.session);
    if (S.instance) xrDestroyInstance(S.instance);
    if (S.egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(S.egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (S.egl_surf) eglDestroySurface(S.egl_dpy, S.egl_surf);
        if (S.egl_ctx)  eglDestroyContext(S.egl_dpy, S.egl_ctx);
        eglTerminate(S.egl_dpy);
    }
    memset(&S, 0, sizeof S);
}
