/*
 * Native Mote device link (USB-CDC) — replaces the Python `mote` CLI's pyserial
 * device commands. Cross-platform: Linux (termios) + Windows (Win32 COM).
 * Finds the board by VID:PID CAFE:4D01 and speaks the framed text protocol:
 *   PING  -> "MOTE <proto>"      LIST -> names... "OK"
 *   PUT <name> <size>\n -> "READY", <size> bytes, "OK"
 *   LAUNCH <name>\n / WIPE\n -> reply       (device also streams LOG lines)
 */
#ifndef MOTE_USB_H
#define MOTE_USB_H

typedef void (*mote_log_fn)(const char *line);   /* sink for output lines */

/* Each returns 0 on success, <0 on error (and logs a human message). */
int  mote_dev_present(void);                                  /* 1 if a board is found */
int  mote_dev_ping(mote_log_fn log);
int  mote_dev_list(mote_log_fn log);

/* Read the device catalog for the gallery diff: each installed game's name
 * (.mote basename, no extension) and version ("0" if the game predates ABI v46).
 * *dev_abi (if non-NULL) gets the device engine's ABI, or 0 if unknown.
 * Returns the game count, or -1 if no device. Does NOT log. */
typedef struct { char name[48]; char version[16]; } MoteCatEntry;
int  mote_dev_catalog(MoteCatEntry *out, int max, int *dev_abi);
int  mote_dev_push(const char *mote_path, const char *name, int launch, mote_log_fn log);
int  mote_dev_wipe(mote_log_fn log);
int  mote_dev_logs(int seconds, mote_log_fn log, volatile int *stop);   /* stream until seconds elapse or *stop */

/* Raw persistent serial pipe (the LAN link bridge relays it to the network). */
void *mote_dev_open_raw(void);                       /* NULL if no device */
int   mote_dev_raw_read(void *h, void *buf, int n);  /* <=~100ms block; 0 = nothing */
int   mote_dev_raw_write(void *h, const void *buf, int n);
void  mote_dev_close_raw(void *h);

#endif
