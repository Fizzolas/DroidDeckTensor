/* LD_PRELOAD ioctl logger. Each call writes one line to /data/local/tmp/ioctl.log:
 *   fd=N req=0x... dir=D size=S type=0x.. nr=0x.. ret=R errno=E arg=<hex of S bytes>
 * The arg bytes are logged after the call, so output fields are visible.
 * Rebuilt from the capture format; the original binary's source was not recovered. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

/* Bionic declares ioctl(int, int, ...); this must match or the build fails. */
typedef int (*ioctl_fn)(int, int, ...);

int ioctl(int fd, int request, ...) {
    static ioctl_fn real;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    unsigned long req = (unsigned long)(unsigned int)request;

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    int ret = real(fd, request, arg);
    int err = errno;

    unsigned dir = (unsigned)((req >> 30) & 3);
    unsigned size = (unsigned)((req >> 16) & 0x3fff);
    unsigned type = (unsigned)((req >> 8) & 0xff);
    unsigned nr = (unsigned)(req & 0xff);

    FILE *f = fopen("/data/local/tmp/ioctl.log", "a");
    if (!f) return ret;
    fprintf(f, "fd=%d req=0x%08lx dir=%u size=%u type=0x%02x nr=0x%02x ret=%d errno=%d arg=",
            fd, req, dir, size, type, nr, ret, err);
    if (arg) {
        const unsigned char *p = arg;
        for (unsigned i = 0; i < size; i++) fprintf(f, "%02x", p[i]);
    }
    fputc('\n', f);
    fclose(f);
    errno = err;
    return ret;
}
