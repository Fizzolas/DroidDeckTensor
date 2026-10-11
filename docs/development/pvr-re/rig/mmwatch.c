/*
 * mmwatch - record every change to the process's memory map, with the caller.
 *
 * The question this exists for: the PowerVR HAL's PLT slots point into libhybris.so when the HAL
 * opens, and into unmapped memory when the client dies. Whether that region was unmapped later or
 * never mapped in that process cannot be told from a crash report - there is no record of the
 * minutes before it. This is that record.
 *
 * Interposes mmap, mmap64, munmap and mprotect, and appends one line per accepted call:
 *
 *   <seq> <pid> <comm> <op> <addr> <len> <prot|-> <result> <errno> caller=<pc> <module+off>
 *
 * The caller is captured as the FIRST instruction of the interposer - x30 is the link register and
 * any call made before reading it (dlsym, open, even formatting) clobbers it.
 *
 * Two things it does carefully, both learned from a version that got them wrong:
 *   - it does not call mmap/munmap while handling a call, so a logging buffer growing is the one
 *     allocation that can re-enter. Buffered output is flushed when it fills and only through write().
 *   - it identifies the module an address is in by reading /proc/self/maps at the time, since the
 *     set of loaded modules changes as the process runs.
 *
 * Build: see build.sh. Output: /tmp/mm.log (append). Size-capped so a runaway session cannot fill
 * the device.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>

#define OUT "/tmp/mm.log"
#define LOG_CAP (64u * 1024u * 1024u)

static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static int (*real_munmap)(void *, size_t);
static int (*real_mprotect)(void *, size_t, int);

static char buf[1 << 16];
static size_t blen;
static unsigned long long seqno;
static long long written;

/* Buffered writer. No allocation: a fixed buffer, flushed with write(2). */
static void wflush(void) {
    if (!blen) return;
    int fd = open(OUT, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ssize_t r = write(fd, buf, blen);
        if (r > 0) written += r;
        close(fd);
    }
    blen = 0;
}
static void put(const char *s, size_t n) {
    if (written > (long long)LOG_CAP) return;
    if (blen + n > sizeof(buf)) wflush();
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf + blen, s, n);
    blen += n;
}
static void puts_(const char *s) { put(s, strlen(s)); }
static void putx(unsigned long long v) {
    char t[19]; int i = 18; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    puts_("0x"); put(t + i, (size_t)(18 - i));
}
static void putd(long long v) {
    char t[24]; int i = 23; t[i] = 0; int neg = v < 0;
    unsigned long long u = neg ? -(unsigned long long)v : (unsigned long long)v;
    if (!u) t[--i] = '0';
    while (u) { t[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) t[--i] = '-';
    put(t + i, (size_t)(23 - i));
}
static void put_comm(void) {
    char cb[64] = {0};
    int f = open("/proc/self/comm", O_RDONLY);
    if (f >= 0) { ssize_t n = read(f, cb, sizeof(cb) - 1); (void)n; close(f); }
    for (char *q = cb; *q; q++) if (*q == 10) { *q = 0; break; }
    puts_(cb[0] ? cb : "?");
}

/* The module owning an address, from a fresh read of maps. Opens the file directly so this never
 * re-enters the interposed mmap. Not cached: modules come and go, which is the point of the log. */
static void put_module(uintptr_t a) {
    static char mb[1 << 19];
    int f = open("/proc/self/maps", O_RDONLY);
    if (f < 0) { puts_("[maps-unreadable]"); return; }
    ssize_t n = read(f, mb, sizeof(mb) - 1);
    close(f);
    if (n <= 0) { puts_("[maps-empty]"); return; }
    mb[n] = 0;
    for (char *q = mb; q && *q; ) {
        char *nl = strchr(q, 10);
        if (nl) *nl = 0;
        unsigned long long lo, hi;
        if (sscanf(q, "%llx-%llx", &lo, &hi) == 2 && a >= lo && a < hi) {
            char *path = strrchr(q, '/');
            if (path) { puts_(path + 1); }
            else { puts_("[anon]"); }
            puts_("+");
            putx((unsigned long long)(a - lo));
            return;
        }
        q = nl ? nl + 1 : NULL;
    }
    puts_("[unmapped]");
}

static void line(const char *op, void *addr, size_t len, int prot, long long res, int err, uintptr_t lr) {
    putd((long long)++seqno); put(" ", 1);
    putd((long long)getpid()); put(" ", 1);
    put_comm(); put(" ", 1);
    puts_(op); put(" ", 1);
    putx((unsigned long long)(uintptr_t)addr); put(" ", 1);
    putx((unsigned long long)len); put(" ", 1);
    if (prot < 0) puts_("-"); else putd(prot);
    put(" res=", 5); putd(res); put(" errno=", 7); putd(err);
    put(" caller=", 8); putx((unsigned long long)lr); put(" ", 1);
    put_module(lr);
    put("\n", 1);
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    uintptr_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    if (!real_mmap) real_mmap = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    void *r = real_mmap(addr, len, prot, flags, fd, off);
    int e = errno;
    if (len >= 0x1000) line("mmap", r, len, prot, (long long)(intptr_t)r, e, lr);
    return r;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*real64)(void *, size_t, int, int, int, off_t);
    uintptr_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    if (!real64) real64 = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap64");
    void *r = real64 ? real64(addr, len, prot, flags, fd, off)
                     : real_mmap(addr, len, prot, flags, fd, off);
    int e = errno;
    if (len >= 0x1000) line("mmap64", r, len, prot, (long long)(intptr_t)r, e, lr);
    return r;
}
int munmap(void *addr, size_t len) {
    uintptr_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    if (!real_munmap) real_munmap = (int (*)(void *, size_t))dlsym(RTLD_NEXT, "munmap");
    int r = real_munmap(addr, len);
    int e = errno;
    if (len >= 0x1000) line("munmap", addr, len, -1, r, e, lr);
    return r;
}
int mprotect(void *addr, size_t len, int prot) {
    uintptr_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    if (!real_mprotect) real_mprotect = (int (*)(void *, size_t, int))dlsym(RTLD_NEXT, "mprotect");
    int r = real_mprotect(addr, len, prot);
    int e = errno;
    if (len >= 0x1000) line("mprotect", addr, len, prot, r, e, lr);
    return r;
}

__attribute__((destructor)) static void fini(void) { wflush(); }
