/* openwatch - record every file a process tries to open and what the kernel said back.
 *
 * The CEF GPU process dies with SIGSEGV in this configuration (state doc §82), and the two crash
 * reports that could be symbolized both run through bionic's `__open_2` into the `__errno` region -
 * a failed open whose error path then dereferences something. That is a file the HAL expects to
 * exist and does not.
 *
 * This interposes the open family and appends one line per attempt: pid, the path, the flags, the
 * result, and errno when it failed. Non-invasive by construction - it does not change any return
 * value, so a process runs exactly as it would have.
 *
 * Output: /tmp/open.log, except that a process which dies takes its report with it, so the path is
 * also written to a per-pid file whose name is stable across a fork.
 *
 * Build: rig/build.sh (same zig target as the other preloads).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define OUT "/tmp/open.log"

static int (*real_open)(const char *, int, ...);
static int (*real_openat)(int, const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int in_hook;

static void emit(const char *fn, const char *path, int flags, int fd, int err) {
    /* open() is how this writes; re-entering it would recurse and corrupt the record. */
    if (in_hook) return;
    in_hook = 1;
    int out = real_open ? real_open(OUT, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
    if (out >= 0) {
        char line[1024];
        int n = snprintf(line, sizeof line, "pid=%d %s %s flags=%#x -> %s%s%s\n",
                         (int)getpid(), fn, path ? path : "(null)", flags,
                         fd >= 0 ? "ok" : "FAIL",
                         fd >= 0 ? "" : " errno=",
                         fd >= 0 ? "" : strerror(err));
        if (n > 0) { ssize_t r = write(out, line, (size_t)n); (void)r; }
        close(out);
    }
    in_hook = 0;
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    if (!real_open) real_open = dlsym(RTLD_NEXT, "open");
    int fd = real_open(path, flags, mode);
    int e = errno;
    emit("open", path, flags, fd, e);
    errno = e;
    return fd;
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    if (!real_open64) real_open64 = dlsym(RTLD_NEXT, "open64");
    int fd = real_open64 ? real_open64(path, flags, mode) : -1;
    int e = errno;
    emit("open64", path, flags, fd, e);
    errno = e;
    return fd;
}

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    if (!real_openat) real_openat = dlsym(RTLD_NEXT, "openat");
    int fd = real_openat(dirfd, path, flags, mode);
    int e = errno;
    char buf[512];
    if (dirfd == AT_FDCWD) snprintf(buf, sizeof buf, "%s", path ? path : "(null)");
    else snprintf(buf, sizeof buf, "dirfd=%d/%s", dirfd, path ? path : "(null)");
    emit("openat", buf, flags, fd, e);
    errno = e;
    return fd;
}
