/*
 * Native Mote build/scaffold/bake — replaces the Python `mote` CLI logic in C.
 * No Python. Still needs a C compiler (gcc / arm-none-eabi-gcc) like any build.
 * Cross-platform (Linux + Windows/MinGW). Output goes through the log callback.
 */
#ifndef MOTE_CORE_H
#define MOTE_CORE_H
#include "usb.h"   /* mote_log_fn */

/* Starter-template archetypes for mc_new (each pre-sizes its arena claims). */
enum { MC_TMPL_3D = 0, MC_TMPL_PHYS = 1, MC_TMPL_2D = 2 };

int  mc_name(const char *dir, char *out, int n);    /* game.toml name= or dir basename */
int  mc_filename(const char *dir, char *out, int n);/* protocol/filesystem-safe module name */
void mc_sanitize(const char *in, char *out, int n);
const char *mc_host_ext(void);                      /* "so" (Linux) / "dll" (Windows) */
int  mc_build(const char *dir, int device, mote_log_fn log);  /* build/<name>.{so|dll} (+ .mote if device) */
int  mc_new(const char *name, int kind, mote_log_fn log);     /* scaffold examples/<name> from an MC_TMPL_* */
int  mc_bake(const char *dir, mote_log_fn log);     /* assets/* -> src/*.h headers */

#endif
