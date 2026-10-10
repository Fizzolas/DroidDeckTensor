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
#include <stdlib.h>
#include <string.h>

#include "bridge_abi.h"

typedef int (*ioctl_fn)(int, int, ...);

/* Reporting must not affect the forwarded call, including its stale errno on success. */
static void capture_loss(const char *operation, int error) {
    int saved = errno;
    fprintf(stderr, BRIDGE_CAPTURE_LOSS_MARKER " %s errno=%d\n", operation, error);
    fflush(stderr);
    errno = saved;
}

/* The character count a shared log format produces, read from that format instead of restated
 * here, so a write that comes up short can be told from a complete one. */
#define LOG_LENGTH(...) snprintf(NULL, 0, __VA_ARGS__)

/* Every log field goes through LOG_WRITE. Its arguments are plain values, read twice: once to
 * measure the expected length, once to write it. */
#define LOG_WRITE(f, write_errno, ...) \
    note_write((f), fprintf((f), __VA_ARGS__), LOG_LENGTH(__VA_ARGS__), (write_errno))

/* The errno of the first write this call failed, sampled at that write: a later write, ferror
 * or fclose must never stand in for it. A write failed when it returned an error, when it came
 * up short of the shared format's own width, or when the stream recorded the failure itself.
 * 0 = no write has failed so far. */
static void note_write(FILE *f, int written, int expected, int *write_errno) {
    if (*write_errno == 0 && (written < 0 || written != expected || ferror(f))) *write_errno = errno;
}

static void close_log(FILE *f, int write_errno) {
    if (write_errno != 0) capture_loss("write", write_errno);
    if (fclose(f) != 0) capture_loss("close", errno);
}

static void dump(FILE *f, int *write_errno, const char *label, const uint8_t *p, uint32_t n) {
    /* One byte is BRIDGE_LOG_BYTE, so its width is the same for the whole dump. */
    const int width = LOG_LENGTH(BRIDGE_LOG_BYTE, 0);
    if (!p) {
        LOG_WRITE(f, write_errno, BRIDGE_LOG_NULL, label);
        return;
    }
    LOG_WRITE(f, write_errno, BRIDGE_LOG_DATA_HEAD, label, n);
    for (uint32_t i = 0; i < n; i++)
        note_write(f, fprintf(f, BRIDGE_LOG_BYTE, p[i]), width, write_errno);
}

int ioctl(int fd, int request, ...) {
    static ioctl_fn real;
    if (!real) real = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");

    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    unsigned req = (unsigned)request;
    int write_errno = 0;
    FILE *f = fopen("/data/local/tmp/bridge.log", "a");
    if (!f) capture_loss("open", errno);

    if (req != BRIDGE_IOCTL_REQ || !arg) {
        int ret = real(fd, request, arg);
        int err = errno;
        if (f) {
            LOG_WRITE(f, &write_errno, BRIDGE_LOG_OTHER "\n", req, ret, err);
            close_log(f, write_errno);
        }
        errno = err;
        return ret;
    }

    const uint8_t *a = arg;
    uint32_t bridge, func, in_size, out_size;
    uint64_t in_ptr, out_ptr;
    memcpy(&bridge, a + BRIDGE_ARG_BRIDGE, 4);
    memcpy(&func, a + BRIDGE_ARG_FUNC, 4);
    memcpy(&in_ptr, a + BRIDGE_ARG_IN_PTR, 8);
    memcpy(&out_ptr, a + BRIDGE_ARG_OUT_PTR, 8);
    memcpy(&in_size, a + BRIDGE_ARG_IN_SIZE, 4);
    memcpy(&out_size, a + BRIDGE_ARG_OUT_SIZE, 4);

    const uint8_t *in = (const uint8_t *)(uintptr_t)in_ptr;
    uint8_t *out = (uint8_t *)(uintptr_t)out_ptr;

    /* The ioctl may modify input too (including when in/out alias). Keep a private
     * pre-call snapshot, but still pass the original argument and pointers through.
     * Snapshot failure must not produce a misleading post-call input record. */
    int before_snapshot_errno = errno;
    uint8_t *in_snapshot = NULL;
    if (f && in && in_size) {
        in_snapshot = malloc(in_size);
        if (in_snapshot) {
            memcpy(in_snapshot, in, in_size);
        } else {
            capture_loss("snapshot", errno);
            close_log(f, write_errno);
            f = NULL;
        }
    }
    errno = before_snapshot_errno;

    int ret = real(fd, request, arg);
    int err = errno;

    if (f) {
        LOG_WRITE(f, &write_errno, BRIDGE_LOG_CALL, bridge, func, ret, err);
        dump(f, &write_errno, "in", in_snapshot ? in_snapshot : in, in_size);
        dump(f, &write_errno, "out", out, out_size);
        /* fputc reports the character it wrote, so only its end-of-file return is a failure. */
        if (fputc('\n', f) == EOF && write_errno == 0) write_errno = errno;
        close_log(f, write_errno);
    }
    free(in_snapshot);
    errno = err;
    return ret;
}
