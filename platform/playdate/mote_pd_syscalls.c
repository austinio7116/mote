/*
 * Mote — Playdate: newlib's system-call hooks. There is no POSIX file system
 * behind newlib on the Playdate (games reach storage through mote->save/kv,
 * which go through the Playdate's file API), so anything that still calls
 * fopen/printf ends here and fails quietly -- the same as on the Thumby.
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

int   _open(const char *p, int f, int m) { (void)p; (void)f; (void)m; errno = ENOENT; return -1; }
int   _close(int fd) { (void)fd; return -1; }
int   _read(int fd, char *b, int n) { (void)fd; (void)b; (void)n; return 0; }
int   _write(int fd, const char *b, int n) { (void)fd; (void)b; return n; }
int   _lseek(int fd, int o, int w) { (void)fd; (void)o; (void)w; return 0; }
int   _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int   _isatty(int fd) { (void)fd; return 1; }
int   _getpid(void) { return 1; }
int   _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void  _exit(int code) { (void)code; for (;;) {} }
int   _unlink(const char *p) { (void)p; errno = ENOENT; return -1; }
int   _link(const char *a, const char *b) { (void)a; (void)b; errno = EMLINK; return -1; }
int   _stat(const char *p, struct stat *st) { (void)p; (void)st; errno = ENOENT; return -1; }
void *_sbrk(int n) { (void)n; errno = ENOMEM; return (void *)-1; }

/* time()/gettimeofday() — a few games seed their RNG from the clock. */
#include <sys/time.h>
#include "pd_api.h"
extern PlaydateAPI *pd;
int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    unsigned int ms = 0;
    unsigned int s = pd ? pd->system->getSecondsSinceEpoch(&ms) : 0;
    if (tv) { tv->tv_sec = s; tv->tv_usec = (suseconds_t)ms * 1000; }
    return 0;
}
