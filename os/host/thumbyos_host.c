/*
 * ThumbyOS — host entry. The PC analogue of the device OS: instead of mapping
 * a .tgm out of flash, it dlopen()s a game module .so, checks its ABI version,
 * hands it the engine jump table, and runs the resident frame loop.
 *
 * This proves the platform thesis on the host: a separately-built native game
 * module, loaded at runtime, drives the resident engine purely through the
 * ABI — exactly what the device loader will do, minus the flash mechanics.
 *
 *   thumbyos_host <game.so>
 */
#include "te_os.h"
#include "te_platform.h"
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game.so>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    void *mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!mod) {
        fprintf(stderr, "thumbyos: dlopen failed: %s\n", dlerror());
        return 1;
    }

    const uint32_t *game_abi = (const uint32_t *)dlsym(mod, "te_game_abi_version");
    TeGameRegisterFn reg = (TeGameRegisterFn)dlsym(mod, "te_game_register");
    if (!game_abi || !reg) {
        fprintf(stderr, "thumbyos: '%s' is not a game module "
                        "(missing te_game_abi_version / te_game_register)\n", path);
        dlclose(mod);
        return 1;
    }
    if (*game_abi > TE_ABI_VERSION) {
        fprintf(stderr, "thumbyos: game ABI v%u newer than engine v%u — refusing\n",
                *game_abi, TE_ABI_VERSION);
        dlclose(mod);
        return 1;
    }

    if (te_plat_init("ThumbyOS (host)") != 0) {
        dlclose(mod);
        return 1;
    }

    TeApi api;
    te_api_fill(&api);
    const TeGameVtbl *vt = reg(&api);
    if (!vt) {
        fprintf(stderr, "thumbyos: game register returned no vtable\n");
        te_plat_shutdown();
        dlclose(mod);
        return 1;
    }

    printf("thumbyos: loaded '%s' (game ABI v%u, engine v%u)\n",
           path, *game_abi, TE_ABI_VERSION);
    te_os_run(&api, vt);

    te_plat_shutdown();
    dlclose(mod);
    return 0;
}
