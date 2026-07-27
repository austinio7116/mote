/*
 * Mote OS — Android entry. The phone's analogue of os/device (flash store +
 * loader) and os/host (dlopen a .so).
 *
 * The device runs games as .mote images mapped from flash; a phone can't execute
 * Cortex-M33 code, so an Android build ships each game as its own real .so —
 * built from the SAME src/game.c with the same ABI, linking no engine code and
 * reaching the engine only through the jump table it is handed. Modules are
 * discovered by filename (libmg_<game>.so) in:
 *
 *   1. the APK's native library dir  — the games bundled with the build
 *   2. <external files>/games/       — modules the user side-loads later
 *
 * Everything above the loader (launcher, engine menu, lobby, save slots) is the
 * unmodified OS, so the phone behaves like the handheld.
 *
 * Runs on a worker thread; the SDL main thread owns the chassis UI.
 */
#include "mote_os.h"
#include "mote_android_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include "mote_ui.h"
#include "mote_plat_android.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <unistd.h>        /* access */

#define MAX_DIRS 4
#define MOD_PREFIX "libmg_"
#define MOD_SUFFIX ".so"

typedef struct {
    char        path[512];
    char        name[MOTE_NAME_MAX];   /* launcher/display name */
    char        stem[40];              /* save-file stem (module basename) */
    const void *icon;                  /* dlsym'd; the .so stays open so it lives */
    uint32_t    abi;
    const char *version;
} Mod;

static Mod  s_mod[MOTE_CATALOG_MAX];
static int  s_nmod;
static char s_dir[MAX_DIRS][400];
static int  s_ndir;

void mote_android_os_add_dir(const char *dir) {
    if (!dir || !dir[0] || s_ndir >= MAX_DIRS) return;
    snprintf(s_dir[s_ndir++], sizeof s_dir[0], "%s", dir);
}

/* Gallery installs (and updates) go to the first writable dir — on Android that's
 * the app's external games folder, which is deliberately scanned BEFORE the
 * read-only copies inside the APK so an installed update wins. */
const char *mote_android_os_install_dir(void) {
    for (int i = 0; i < s_ndir; i++)
        if (access(s_dir[i], W_OK) == 0) return s_dir[i];
    return NULL;
}

const char *mote_android_os_installed_version(const char *id) {
    if (!id || !id[0]) return NULL;
    for (int i = 0; i < s_nmod; i++)
        if (!strcasecmp(s_mod[i].stem, id))
            return s_mod[i].version ? s_mod[i].version : "0";
    return NULL;
}

/* Fallback display name from the module filename: libmg_thumbycraft.so -> thumbycraft */
static void name_from_file(const char *file, char *out, int cap) {
    const char *b = file + strlen(MOD_PREFIX);
    int n = 0;
    for (; b[n] && n < cap - 1 && strcmp(b + n, MOD_SUFFIX) != 0; n++) out[n] = b[n];
    out[n] = 0;
}

/* Open a module just far enough to read its metadata. The handle is deliberately
 * kept (refcounted): the launcher blits the icon straight out of the mapped image,
 * exactly as the device blits it from XIP flash. */
static void probe(const char *path, const char *file) {
    if (s_nmod >= MOTE_CATALOG_MAX) return;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { mote_plat_log(dlerror()); return; }
    const uint32_t *abi = dlsym(h, "mote_game_abi_version");
    if (!abi || !dlsym(h, "mote_game_register")) { dlclose(h); return; }

    Mod *m = &s_mod[s_nmod];
    snprintf(m->path, sizeof m->path, "%s", path);
    m->abi = *abi;
    m->icon = dlsym(h, "mote_game_icon_data");
    m->version = (const char *)dlsym(h, "mote_game_version");
    name_from_file(file, m->stem, sizeof m->stem);
    const char *const *nm = dlsym(h, "mote_game_name");
    if (nm && *nm) snprintf(m->name, sizeof m->name, "%s", *nm);
    else           snprintf(m->name, sizeof m->name, "%s", m->stem);
    s_nmod++;
}

static int by_name(const void *a, const void *b) {
    return strcasecmp(((const Mod *)a)->name, ((const Mod *)b)->name);
}

/* Scan the search dirs once at boot. Later dirs never shadow earlier ones, so a
 * side-loaded module with the same filename as a bundled one is skipped. */
static void scan(void) {
    for (int d = 0; d < s_ndir; d++) {
        DIR *dp = opendir(s_dir[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp))) {
            size_t l = strlen(e->d_name);
            if (l <= strlen(MOD_PREFIX) + strlen(MOD_SUFFIX)) continue;
            if (strncmp(e->d_name, MOD_PREFIX, strlen(MOD_PREFIX)) != 0) continue;
            if (strcmp(e->d_name + l - strlen(MOD_SUFFIX), MOD_SUFFIX) != 0) continue;
            int dup = 0;
            for (int i = 0; i < s_nmod && !dup; i++) {
                char stem[40]; name_from_file(e->d_name, stem, sizeof stem);
                dup = (strcmp(s_mod[i].stem, stem) == 0);
            }
            if (dup) continue;
            char path[512];
            snprintf(path, sizeof path, "%s/%s", s_dir[d], e->d_name);
            probe(path, e->d_name);
        }
        closedir(dp);
    }
    if (s_nmod > 1) qsort(s_mod, (size_t)s_nmod, sizeof s_mod[0], by_name);
    char m[64]; snprintf(m, sizeof m, "[mote] %d game module(s) installed", s_nmod);
    mote_plat_log(m);
}

/* A gallery install lands a new (or newer) module while the launcher is up — the
 * device equivalent is a game arriving over USB. Drop any existing entry with the
 * same stem, probe the new file and re-sort, so the catalog the launcher rebuilds
 * next frame already has it. */
int mote_android_os_add_module(const char *path) {
    if (!path || !path[0]) return -1;
    const char *file = strrchr(path, '/');
    file = file ? file + 1 : path;
    if (strncmp(file, MOD_PREFIX, strlen(MOD_PREFIX)) != 0) return -1;

    char stem[40]; name_from_file(file, stem, sizeof stem);
    for (int i = 0; i < s_nmod; i++)
        if (strcmp(s_mod[i].stem, stem) == 0) {
            s_mod[i] = s_mod[--s_nmod];      /* order is restored by the sort below */
            break;
        }
    int before = s_nmod;
    probe(path, file);
    if (s_nmod == before) return -1;          /* not a loadable game module */
    if (s_nmod > 1) qsort(s_mod, (size_t)s_nmod, sizeof s_mod[0], by_name);
    return 0;
}

/* The launcher re-reads the catalog every frame (games can appear at any time on
 * the device); here it's a fixed snapshot of the scan. */
static void fill(MoteCatalog *c) {
    c->count = 0;
    for (int i = 0; i < s_nmod; i++) {
        MoteGameEntry *e = &c->e[c->count++];
        snprintf(e->name, sizeof e->name, "%s", s_mod[i].name);
        e->offset = 0; e->size = 0; e->frag = 0;
        e->icon = 0; e->icon_blob = 0;
        if (s_mod[i].icon) {
            if (s_mod[i].abi >= 22u) e->icon_blob = s_mod[i].icon;
            else                     e->icon = (const uint16_t *)s_mod[i].icon;
        }
    }
}

static void run_game(const Mod *m) {
    void *h = dlopen(m->path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { mote_plat_log(dlerror()); return; }
    const uint32_t *abi = dlsym(h, "mote_game_abi_version");
    MoteGameRegisterFn reg = (MoteGameRegisterFn)dlsym(h, "mote_game_register");
    if (!abi || !reg) { mote_plat_log("[mote] not a game module"); dlclose(h); return; }
    if (*abi > MOTE_ABI_VERSION) {
        char b[96];
        snprintf(b, sizeof b, "[mote] %s needs engine ABI v%u (this build is v%u)",
                 m->name, *abi, MOTE_ABI_VERSION);
        mote_plat_log(b);
        dlclose(h);
        return;
    }
    MoteApi api;
    mote_api_fill(&api);
    const MoteGameVtbl *vt = reg(&api);
    if (!vt) { dlclose(h); return; }

    mote_plat_set_save_game(m->stem);       /* saves + kv are per game */
    mote_shell_set_game_label(m->name);     /* relay Browse label */
    mote_plat_audio_start();
    { char b[96]; snprintf(b, sizeof b, "[mote] launching %s (ABI v%u)", m->name, *abi);
      mote_plat_log(b); }
    mote_shell_set_in_game(1);
    mote_os_run(&api, vt);                  /* returns on exit_to_launcher */
    mote_shell_set_in_game(0);
    mote_shell_take_exit_game();            /* the panel's BACK is not a real quit */
    dlclose(h);
}

int mote_android_os_main(void) {
    if (mote_plat_init("Mote") != 0) return 1;
    scan();
    /* Dev/CI: boot straight into one game by module name, skipping the launcher
     * (the shell's analogue of the host loader's MOTE_AUTORUN). */
    {
        const char *want = getenv("MOTE_SHELL_AUTORUN");
        if (want && want[0]) {
            for (int i = 0; i < s_nmod; i++)
                if (!strcasecmp(s_mod[i].stem, want) || !strcasecmp(s_mod[i].name, want)) {
                    run_game(&s_mod[i]);
                    mote_plat_shutdown();
                    return 0;
                }
            mote_plat_log("[mote] MOTE_SHELL_AUTORUN: no such module");
        }
    }
    for (;;) {
        int idx = mote_launcher_run(fill);
        if (mote_shell_take_exit_game()) continue;      /* stray BACK: stay in the list */
        if (idx == MOTE_LAUNCHER_GALLERY) { mote_android_gallery_screen(); continue; }
        if (idx < 0) break;                            /* window closed / quit */
        if (idx < s_nmod) run_game(&s_mod[idx]);
    }
    mote_plat_shutdown();
    return 0;
}
