/*
 * Mote OS — Android entry (see mote_android_os.c). The shell adds the module
 * search dirs, then runs this on a worker thread; it owns the launcher and the
 * per-game frame loop and returns when the shell asks to quit.
 */
#ifndef MOTE_ANDROID_OS_H
#define MOTE_ANDROID_OS_H

/* Where to look for libmg_<game>.so modules, highest priority first. */
void mote_android_os_add_dir(const char *dir);

/* Launcher -> game -> launcher, until mote_shell_request_quit(). */
int  mote_android_os_main(void);

#endif /* MOTE_ANDROID_OS_H */
