/* LD_PRELOAD bridge logger. The PowerVR bridge ioctl (req 0xc0206440) takes a 32-byte
 * argument: { u32 bridge; u32 func; u64 in_ptr; u64 out_ptr; u32 in_size; u32 out_size }.
 * For each such call this writes one B line with the input bytes before the call and the
 * output bytes after it. Every other ioctl writes a D line. Output goes to
 * /data/local/tmp/bridge.log in the format the decode script reads. Rebuilt from the
 * capture format; the original binary's source was not recovered. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int (*ioctl_fn)(int, int, ...);

#define BRIDGE_REQ 0xc0206440u

static void dump(FILE *f, const char *label, const uint8_t *p, uint32_t n) {
    if (!p) {
        fprintf(f, " %s=NULL", label);
        return;
    }
    fprintf(f, " %s[%u]=", label, n);
    for (uint32_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]);
}

int ioctl(int fd, int request, ...) {
    static ioctl_fn real;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    unsigned req = (unsigned)request;
    FILE *f = fopen("/data/local/tmp/bridge.log", "a");

    if (req != BRIDGE_REQ || !arg) {
        int ret = real(fd, request, arg);
        int err = errno;
        if (f) {
            fprintf(f, "D req=0x%08x ret=%d errno=%d\n", req, ret, err);
            fclose(f);
        }
        errno = err;
        return ret;
    }

    const uint8_t *a = arg;
    uint32_t bridge, func, in_size, out_size;
    uint64_t in_ptr, out_ptr;
    memcpy(&bridge, a + 0, 4);
    memcpy(&func, a + 4, 4);
    memcpy(&in_ptr, a + 8, 8);
    memcpy(&out_ptr, a + 16, 8);
    memcpy(&in_size, a + 24, 4);
    memcpy(&out_size, a + 28, 4);

    const uint8_t *in = (const uint8_t *)(uintptr_t)in_ptr;
    uint8_t *out = (uint8_t *)(uintptr_t)out_ptr;

    int ret = real(fd, request, arg);
    int err = errno;

    if (f) {
        fprintf(f, "B bridge=%u func=%u ret=%d errno=%d", bridge, func, ret, err);
        dump(f, "in", in, in_size);
        if (out) dump(f, "out", out, out_size);
        else fprintf(f, " out=NULL");
        fputc('\n', f);
        fclose(f);
    }
    errno = err;
    return ret;
}
