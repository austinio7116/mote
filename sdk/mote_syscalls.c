/*
 * Mote — minimal libc syscall stubs for device (.mote) modules.
 *
 * A game module is bare-metal: linked with -nostartfiles and no OS libc, so the
 * usual newlib syscall back-ends (_read/_write/_sbrk/…) are absent. Newlib's
 * snprintf/printf family still REFERENCE them through its reentrant FILE
 * machinery, even when only formatting into a caller-supplied buffer. These
 * stubs satisfy the linker; none are reached at runtime because a game never
 * performs real file I/O (and uses mote->alloc, not malloc, for memory).
 *
 * Built and linked only for the device path (see tools/mote build_device).
 */
#include <errno.h>
#include <sys/stat.h>

int   _read(int fd, char *buf, int n)        { (void)fd; (void)buf; (void)n; errno = ENOSYS; return -1; }
int   _write(int fd, const char *buf, int n) { (void)fd; (void)buf; return n; }   /* pretend success */
int   _close(int fd)                         { (void)fd; errno = ENOSYS; return -1; }
int   _lseek(int fd, int off, int whence)    { (void)fd; (void)off; (void)whence; errno = ENOSYS; return -1; }
int   _fstat(int fd, struct stat *st)        { (void)fd; if (st) st->st_mode = S_IFCHR; return 0; }
int   _isatty(int fd)                        { (void)fd; return 1; }
void *_sbrk(int incr)                        { (void)incr; errno = ENOMEM; return (void *)-1; }
int   _getpid(void)                          { return 1; }
int   _kill(int pid, int sig)                { (void)pid; (void)sig; errno = ENOSYS; return -1; }
void  _exit(int code)                        { (void)code; for (;;) {} }
