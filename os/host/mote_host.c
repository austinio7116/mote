/*
 * Mote OS — host entry. The PC analogue of the device OS.
 *
 * Takes a set of game-module .so paths (the host's stand-in for the on-flash
 * game store), shows the launcher, and on selection dlopen()s that module,
 * hands it the engine jump table, and runs the resident frame loop. On exit
 * (the game calls exit_to_launcher) it returns to the launcher. dlopen here is
 * the analogue of the device's ATRANS map.
 *
 *   mote_host <game1.so> [game2.so ...]
 */
#include "mote_os.h"
#include "mote_launcher.h"
#include "mote_platform.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Pretty name from a path: strip dir and trailing ".so". */
static void nice_name(const char *path, char *out, int cap) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int n = 0;
    for (; base[n] && n < cap - 1; n++) out[n] = base[n];
    out[n] = 0;
    char *dot = strstr(out, ".so");
    if (dot) *dot = 0;
}

static int run_game(const char *path) {
    void *mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!mod) { fprintf(stderr, "mote: dlopen failed: %s\n", dlerror()); return 1; }

    const uint32_t *game_abi = (const uint32_t *)dlsym(mod, "mote_game_abi_version");
    MoteGameRegisterFn reg = (MoteGameRegisterFn)dlsym(mod, "mote_game_register");
    if (!game_abi || !reg) {
        fprintf(stderr, "mote: '%s' is not a game module\n", path);
        dlclose(mod); return 1;
    }
    if (*game_abi > MOTE_ABI_VERSION) {
        fprintf(stderr, "mote: game ABI v%u > engine v%u — refusing\n",
                *game_abi, MOTE_ABI_VERSION);
        dlclose(mod); return 1;
    }

    MoteApi api;
    mote_api_fill(&api);
    const MoteGameVtbl *vt = reg(&api);
    if (!vt) { dlclose(mod); return 1; }

    printf("mote: launching '%s' (ABI v%u)\n", path, *game_abi);
    mote_os_run(&api, vt);     /* returns on exit_to_launcher */
    dlclose(mod);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game1.so> [game2.so ...]\n", argv[0]);
        return 2;
    }

    int n = argc - 1;
    if (n > MOTE_CATALOG_MAX) n = MOTE_CATALOG_MAX;

    MoteCatalog cat;
    cat.count = n;
    for (int i = 0; i < n; i++)
        nice_name(argv[i + 1], cat.e[i].name, MOTE_NAME_MAX);

    if (mote_plat_init("Mote OS (host)") != 0) return 1;

    for (;;) {
        int idx = mote_launcher_run(&cat);
        if (idx < 0) break;                 /* window closed */
        run_game(argv[idx + 1]);
    }

    mote_plat_shutdown();
    return 0;
}
