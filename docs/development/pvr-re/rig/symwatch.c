/*
 * symwatch - record what libhybris's hook layer is asked to resolve, and to what.
 *
 * The HAL is bionic, so its imports go through libhybris's hook layer: for each symbol the loader
 * resolves on the HAL's behalf, `__hybris_get_hooked_symbol` is consulted, and what it returns becomes
 * the PLT slot's target. A slot that points somewhere wrong - or that pointed somewhere valid at
 * open() and into nothing at the fault - is only explicable with a record of that decision.
 *
 * This installs a callback with libhybris's `hybris_set_hook_callback`, which is the documented
 * extension point (`hybris/common/hooks.h`), and writes one line per symbol:
 *
 *   <pid> <comm> sym=<name> requester=<module> caller=<pc> <module+off>
 *
 * It deliberately does NOT interpose dlsym/dlopen: an earlier version did, and resolving dlsym from
 * inside dlsym is a recursion hazard that can take the whole session down. The callback is the
 * supported seam and it is enough - the hook decision is what the PLT slot ends up holding.
 *
 * `hybris_set_hook_callback` is a single slot, so whichever component sets it last wins; the ICD sets
 * its own for the same purpose. Load this OR use the ICD's, not both.
 *
 * Output: /tmp/sym.log (append).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>

#define OUT "/tmp/sym.log"
#define LOG_CAP (32u * 1024u * 1024u)

static char buf[1 << 16];
static size_t blen;
static long long written;

static void wflush(void) {
    if (!blen) return;
    int fd = open(OUT, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { ssize_t r = write(fd, buf, blen); if (r > 0) written += r; close(fd); }
    blen = 0;
}
static void put(const char *s, size_t n) {
    if (written > (long long)LOG_CAP || !s) return;
    if (blen + n > sizeof(buf)) wflush();
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf + blen, s, n); blen += n;
}
static void puts_(const char *s) { if (s) put(s, strlen(s)); }
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
static void put_module(uintptr_t a) {
    static char mb[1 << 19];
    int f = open("/proc/self/maps", O_RDONLY);
    if (f < 0) { puts_("[maps-unreadable]"); return; }
    ssize_t n = read(f, mb, sizeof(mb) - 1); close(f);
    if (n <= 0) { puts_("[maps-empty]"); return; }
    mb[n] = 0;
    for (char *q = mb; q && *q; ) {
        char *nl = strchr(q, 10); if (nl) *nl = 0;
        unsigned long long lo, hi;
        if (sscanf(q, "%llx-%llx", &lo, &hi) == 2 && a >= lo && a < hi) {
            char *path = strrchr(q, '/');
            puts_(path ? path + 1 : "[anon]"); puts_("+"); putx(a - lo);
            return;
        }
        q = nl ? nl + 1 : NULL;
    }
    puts_("[unmapped]");
}

typedef void *(*hook_cb_t)(const char *name, const char *requester);

static void *sym_hook_cb(const char *name, const char *requester) {
    uintptr_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    putd((long long)getpid()); put(" ", 1); put_comm();
    put(" sym=", 5); puts_(name);
    put(" requester=", 11); puts_(requester ? requester : "(null)");
    put(" caller=", 8); putx((unsigned long long)lr); put(" ", 1); put_module(lr);
    put("\n", 1);
    wflush();
    return NULL;   /* NULL: let libhybris resolve it through its own tables, unchanged */
}

__attribute__((constructor)) static void install(void) {
    hook_cb_t (*set)(hook_cb_t) =
        (hook_cb_t (*)(hook_cb_t))dlsym(RTLD_DEFAULT, "hybris_set_hook_callback");
    if (set) set(sym_hook_cb);
    int f = open("/tmp/symwatch.loaded", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) {
        const char *m = set ? "loaded+registered\n" : "loaded+no-symbol\n";
        ssize_t r = write(f, m, strlen(m)); (void)r; close(f);
    }
}
__attribute__((destructor)) static void fini(void) { wflush(); }
