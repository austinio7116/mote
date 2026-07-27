/*
 * Mote OS — Android entry (see mote_android_os.c). The shell adds the module
 * search dirs, then runs this on a worker thread; it owns the launcher and the
 * per-game frame loop and returns when the shell asks to quit.
 */
#ifndef MOTE_ANDROID_OS_H
#define MOTE_ANDROID_OS_H

/* Where to look for libmg_<game>.so modules, highest priority first. The first
 * writable dir added is also where gallery installs land. */
void mote_android_os_add_dir(const char *dir);

/* Launcher -> game -> launcher, until mote_shell_request_quit(). */
int  mote_android_os_main(void);

/* ---- used by the gallery screen (mote_android_gallery.c) ---------------- */
/* Writable module dir (gallery installs), or NULL if every dir is read-only. */
const char *mote_android_os_install_dir(void);
/* MOTE_GAME_VERSION of the installed module whose stem is `id`, or NULL. */
const char *mote_android_os_installed_version(const char *id);
/* Add (or replace) a module by path and re-sort the catalog, so a freshly
 * installed game appears in the launcher without a relaunch. 0 on success. */
int  mote_android_os_add_module(const char *path);

/* The gallery screen itself (RB in the launcher). */
void mote_android_gallery_screen(void);
void mote_android_gallery_set_base(const char *base);   /* NULL = compiled default */

#endif /* MOTE_ANDROID_OS_H */
