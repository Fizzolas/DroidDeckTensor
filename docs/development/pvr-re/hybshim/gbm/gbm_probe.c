/*
 * libgbm.so.1 probe shim - matched-control variant of the AHB shim.
 *
 * One binary for two experiments, selected at runtime by env GBM_SHIM_MODE so the logging is
 * byte-identical between them:
 *   nulldev : gbm_create_device returns NULL (the §56-57 baseline; CEF's non-GBM fallback).
 *   stub    : gbm_create_device returns a real shim-owned device and every device QUERY works
 *             honestly, but every ALLOCATION and IMPORT fails explicitly (NULL + errno) -
 *             the Experiment-2 "valid device, no buffer allocation" row.
 *
 * Every entry point logs: monotonic ms, pid.tid, function, key args, caller module+file-offset
 * (dladdr of the return address), return value, errno. No caller-owned fd is ever closed here.
 * No capability is advertised that this shim cannot honor in the active mode.
 *
 * Log: /root/.local/share/Steam/logs/gbma.log (append; opened O_CLOEXEC - a Chromium GPU-process
 * crash was earlier observed closing stray low fds, so no inherited descriptors).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syscall.h>
#include <time.h>
#include <unistd.h>

/* ---- logging --------------------------------------------------------------------------- */
static int logfd = -1;
static int g_mode;              /* 0 = nulldev, 1 = stub */
static const char *g_selfpath = "?";
static char g_role[64] = "browser";   /* cached Chromium --type= role, for every log line */
static char g_comm[64] = "?";

static const char *proc_role(void) {
    /* Chromium passes --type=<role> to every child; the browser process has none. */
    static int done;
    if (done) return g_role;
    done = 1;
    int f = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (f < 0) return g_role;
    static char cl[8192];
    ssize_t r = read(f, cl, sizeof(cl) - 1);
    close(f);
    if (r <= 0) return g_role;
    cl[r] = 0;
    const char *p = cl, *e = cl + r;
    while (p < e) {
        size_t l = strlen(p);
        if (strncmp(p, "--type=", 7) == 0) {
            const char *v = p + 7;
            size_t vl = strnlen(v, (size_t)(e - v));
            if (vl >= sizeof(g_role)) vl = sizeof(g_role) - 1;
            memcpy(g_role, v, vl);
            g_role[vl] = 0;
            break;
        }
        p += l + 1;
    }
    return g_role;
}

static void log_init(void) {
    if (logfd >= 0) return;
    const char *m = getenv("GBM_SHIM_MODE");
    g_mode = (m && strcmp(m, "stub") == 0) ? 1 : 0;
    logfd = open("/root/.local/share/Steam/logs/gbma.log",
                 O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
}

/* caller description: module basename + offset into it */
static void caller_str(void *ra, char *out, size_t n) {
    Dl_info ci;
    if (ra && dladdr(ra, &ci) && ci.dli_fname && ci.dli_fbase) {
        const char *base = strrchr(ci.dli_fname, '/');
        base = base ? base + 1 : ci.dli_fname;
        snprintf(out, n, "%s+0x%lx", base,
                 (unsigned long)((uintptr_t)ra - (uintptr_t)ci.dli_fbase));
    } else {
        snprintf(out, n, "ra=%p", ra);
    }
}

static void log_fmt(void *ra, const char *fmt, ...) {
    if (logfd < 0) return;
    struct timespec wall, mono;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    char msg[512], call[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    caller_str(ra, call, sizeof(call));
    char line[768];
    /* epoch ms FIRST so lines correlate with Steam's wall-clock logs; monotonic kept after. */
    int len = snprintf(line, sizeof(line), "%lld.%03ld mono=%lld.%03ld %d.%d %s/%s [%s] %s\n",
                       (long long)wall.tv_sec, wall.tv_nsec / 1000000,
                       (long long)mono.tv_sec, mono.tv_nsec / 1000000,
                       (int)getpid(), (int)syscall(SYS_gettid), g_comm, proc_role(), call, msg);
    if (len > 0) { ssize_t w = write(logfd, line, (size_t)len); (void)w; }
}
#define LOG(fmt, ...) log_fmt(__builtin_return_address(0), fmt, ##__VA_ARGS__)

/* honest path of an fd passed in by the caller (readlink /proc/self/fd/N; never take ownership) */
static void fd_path(int fd, char *out, size_t n) {
    char link[64];
    out[0] = 0;
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t r = readlink(link, out, n - 1);
    if (r > 0) out[r] = 0; else out[0] = 0;
}

/* ---- objects ----------------------------------------------------------------------------- */
struct gbm_device { int fd; char devpath[128]; uint32_t magic; };
#define GBM_DEV_MAGIC 0x42444247u  /* "GBDB" */
struct gbm_bo { uint32_t magic; };
struct gbm_surface { uint32_t magic; };
struct gbm_bo_real { struct gbm_bo base; };  /* never returned by this probe */

static int dev_ok(struct gbm_device *d) { return d && d->magic == GBM_DEV_MAGIC; }

/* ---- device ------------------------------------------------------------------------------ */
struct gbm_device *gbm_create_device(int fd) {
    log_init();
    char p[128];
    fd_path(fd, p, sizeof(p));
    LOG("gbm_create_device(fd=%d '%s') mode=%s", fd, p, g_mode ? "stub" : "nulldev");
    if (!g_mode) { errno = ENODEV; LOG("  -> NULL errno=ENODEV"); return NULL; }
    struct gbm_device *d = calloc(1, sizeof(*d));
    if (!d) { errno = ENOMEM; LOG("  -> NULL errno=ENOMEM"); return NULL; }
    d->fd = fd; d->magic = GBM_DEV_MAGIC;
    snprintf(d->devpath, sizeof(d->devpath), "%s", p);
    LOG("  -> dev=%p (stub device; allocations will fail)", (void *)d);
    return d;
}
void gbm_device_destroy(struct gbm_device *d) {
    log_init();
    LOG("gbm_device_destroy(dev=%p ok=%d)", (void *)d, dev_ok(d));
    if (dev_ok(d)) { d->magic = 0; free(d); }
}
const char *gbm_device_get_backend_name(struct gbm_device *d) {
    log_init();
    LOG("gbm_device_get_backend_name(dev=%p ok=%d) -> \"droiddeck-probe\"", (void *)d, dev_ok(d));
    return "droiddeck-probe";
}
int gbm_device_get_fd(struct gbm_device *d) {
    log_init();
    int r = dev_ok(d) ? d->fd : -1;
    LOG("gbm_device_get_fd(dev=%p) -> %d (devpath '%s')", (void *)d, r, dev_ok(d) ? d->devpath : "");
    return r;
}
int gbm_device_is_format_supported(struct gbm_device *d, uint32_t format, uint32_t usage) {
    log_init();
    /* Honest: this probe never allocates or imports, so it advertises nothing. (Advertising the
     * 8888 family here would claim a buffer the probe then cannot create or import.) */
    LOG("gbm_device_is_format_supported(dev=%p ok=%d fmt=0x%x usage=0x%x) -> 0 (probe allocates nothing)",
        (void *)d, dev_ok(d), format, usage);
    return 0;
}
uint32_t gbm_device_get_format_modifier_plane_count(struct gbm_device *d, uint32_t format, uint64_t modifier) {
    log_init();
    (void)format; (void)modifier;
    LOG("gbm_device_get_format_modifier_plane_count(dev=%p fmt=0x%x mod=%llu) -> 0",
        (void *)d, format, (unsigned long long)modifier);
    return 0;
}

/* ---- buffer objects: every allocation/import fails explicitly ---------------------------- */
#define GBM_BO_IMPORT_WL_BUFFER     0x5501
#define GBM_BO_IMPORT_EGL_IMAGE     0x5502
#define GBM_BO_IMPORT_FD            0x5503
#define GBM_BO_IMPORT_FD_MODIFIER   0x5504
struct gbm_import_fd_data { int fd; uint32_t width, height, stride, format; };
struct gbm_import_fd_modifier_data {
    uint32_t width, height, format, num_planes;
    int fds[4]; int strides[4]; int offsets[4]; uint64_t modifier;
};

static struct gbm_bo *fail_bo(const char *who) {
    log_init();
    LOG("%s -> NULL errno=ENOSYS", who);
    errno = ENOSYS;   /* explicit: probe shim never allocates */
    return NULL;
}
struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    log_init();
    LOG("gbm_bo_create(dev=%p ok=%d %ux%u fmt=0x%x flags=0x%x)", (void *)d, dev_ok(d), w, h, fmt, flags);
    return fail_bo("gbm_bo_create");
}
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt,
                                            const uint64_t *mods, unsigned n) {
    log_init();
    LOG("gbm_bo_create_with_modifiers(dev=%p %ux%u fmt=0x%x nmods=%u mod0=%s)",
        (void *)d, w, h, fmt, n, (mods && n) ? "seen" : "none");
    return fail_bo("gbm_bo_create_with_modifiers");
}
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt,
                                             const uint64_t *mods, unsigned n, uint32_t flags) {
    log_init();
    LOG("gbm_bo_create_with_modifiers2(dev=%p %ux%u fmt=0x%x nmods=%u flags=0x%x)",
        (void *)d, w, h, fmt, n, flags);
    return fail_bo("gbm_bo_create_with_modifiers2");
}
struct gbm_bo *gbm_bo_import(struct gbm_device *d, uint32_t type, void *buffer, uint32_t usage) {
    log_init();
    if (!dev_ok(d) || !buffer) { LOG("gbm_bo_import(dev=%p type=0x%x buffer=%p) bad", (void *)d, type, buffer); return fail_bo("gbm_bo_import"); }
    if (type == GBM_BO_IMPORT_FD) {
        struct gbm_import_fd_data *im = buffer;
        char p[128]; fd_path(im->fd, p, sizeof(p));
        LOG("gbm_bo_import(FD) w=%u h=%u stride=%u fmt=0x%x fd=%d '%s' usage=0x%x (caller fd not closed)",
            im->width, im->height, im->stride, im->format, im->fd, p, usage);
    } else if (type == GBM_BO_IMPORT_FD_MODIFIER) {
        struct gbm_import_fd_modifier_data *im = buffer;
        LOG("gbm_bo_import(FD_MODIFIER) w=%u h=%u fmt=0x%x planes=%u fd0=%d stride0=%u off0=%u mod=%llu usage=0x%x",
            im->width, im->height, im->format, im->num_planes, im->fds[0], im->strides[0], im->offsets[0],
            (unsigned long long)im->modifier, usage);
    } else {
        LOG("gbm_bo_import(type=0x%x buffer=%p usage=0x%x)", type, buffer, usage);
    }
    return fail_bo("gbm_bo_import");
}
void gbm_bo_destroy(struct gbm_bo *bo) { log_init(); LOG("gbm_bo_destroy(bo=%p)", (void *)bo); }

/* accessors: only reachable if a bo was returned - it never is; log defensively */
static uint32_t g_zero_u(const char *who, void *bo) { log_init(); LOG("%s(bo=%p) -> 0", who, bo); return 0; }
uint32_t gbm_bo_get_width(struct gbm_bo *b)  { return g_zero_u("gbm_bo_get_width", b); }
uint32_t gbm_bo_get_height(struct gbm_bo *b) { return g_zero_u("gbm_bo_get_height", b); }
uint32_t gbm_bo_get_stride(struct gbm_bo *b) { return g_zero_u("gbm_bo_get_stride", b); }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *b, int p) { (void)p; return g_zero_u("gbm_bo_get_stride_for_plane", b); }
uint32_t gbm_bo_get_format(struct gbm_bo *b) { return g_zero_u("gbm_bo_get_format", b); }
uint32_t gbm_bo_get_bpp(struct gbm_bo *b)    { return g_zero_u("gbm_bo_get_bpp", b); }
uint64_t gbm_bo_get_modifier(struct gbm_bo *b) { log_init(); LOG("gbm_bo_get_modifier(bo=%p) -> 0", (void *)b); return 0; }
int gbm_bo_get_plane_count(struct gbm_bo *b) { return (int)g_zero_u("gbm_bo_get_plane_count", b); }
uint32_t gbm_bo_get_offset(struct gbm_bo *b, int p) { (void)p; return g_zero_u("gbm_bo_get_offset", b); }
int gbm_bo_get_fd(struct gbm_bo *b) { log_init(); LOG("gbm_bo_get_fd(bo=%p) -> -1", (void *)b); return -1; }
int gbm_bo_get_fd_for_plane(struct gbm_bo *b, int p) { (void)p; log_init(); LOG("gbm_bo_get_fd_for_plane(bo=%p) -> -1", (void *)b); return -1; }
int gbm_bo_get_plane_fd(struct gbm_bo *b, int p) { (void)p; log_init(); LOG("gbm_bo_get_plane_fd(bo=%p) -> -1", (void *)b); return -1; }
size_t gbm_bo_get_plane_size(struct gbm_bo *b, int p) { (void)p; return g_zero_u("gbm_bo_get_plane_size", b); }
void *gbm_bo_get_device(struct gbm_bo *b) { log_init(); LOG("gbm_bo_get_device(bo=%p) -> NULL", (void *)b); return NULL; }
union gbm_bo_handle { void *ptr; int32_t s32; uint32_t u32; uint64_t u64; };
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *b) { union gbm_bo_handle h = {0}; log_init(); LOG("gbm_bo_get_handle(bo=%p) -> {0}", (void *)b); return h; }
union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *b, int p) { (void)p; union gbm_bo_handle h = {0}; log_init(); LOG("gbm_bo_get_handle_for_plane(bo=%p) -> {0}", (void *)b); return h; }
void *gbm_bo_map(struct gbm_bo *b, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t f, uint32_t *st, void **md) {
    (void)x;(void)y;(void)w;(void)h;(void)f;(void)st;(void)md;
    log_init(); LOG("gbm_bo_map(bo=%p) -> NULL", (void *)b); return NULL;
}
void gbm_bo_unmap(struct gbm_bo *b, void *md) { (void)md; log_init(); LOG("gbm_bo_unmap(bo=%p)", (void *)b); }
int gbm_bo_write(struct gbm_bo *b, const void *buf, size_t c) { (void)buf;(void)c; log_init(); LOG("gbm_bo_write(bo=%p) -> -1", (void *)b); return -1; }
void gbm_bo_set_user_data(struct gbm_bo *b, void *d, void (*f)(struct gbm_bo *, void *)) { (void)d;(void)f; log_init(); LOG("gbm_bo_set_user_data(bo=%p)", (void *)b); }
void *gbm_bo_get_user_data(struct gbm_bo *b) { log_init(); LOG("gbm_bo_get_user_data(bo=%p) -> NULL", (void *)b); return NULL; }

/* ---- surfaces ---------------------------------------------------------------------------- */
struct gbm_surface *gbm_surface_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    log_init(); LOG("gbm_surface_create(dev=%p %ux%u fmt=0x%x flags=0x%x) -> NULL", (void *)d, w, h, fmt, flags); errno = ENOSYS; return NULL;
}
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n) {
    log_init(); LOG("gbm_surface_create_with_modifiers(dev=%p %ux%u fmt=0x%x nmods=%u) -> NULL", (void *)d, w, h, fmt, n); errno = ENOSYS; return NULL;
}
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n, uint32_t f) {
    log_init(); LOG("gbm_surface_create_with_modifiers2(dev=%p %ux%u nmods=%u flags=0x%x) -> NULL", (void *)d, w, h, n, f); errno = ENOSYS; return NULL;
}
void gbm_surface_destroy(struct gbm_surface *s) { log_init(); LOG("gbm_surface_destroy(s=%p)", (void *)s); }
int gbm_surface_has_free_buffers(struct gbm_surface *s) { (void)s; log_init(); LOG("gbm_surface_has_free_buffers -> 0"); return 0; }
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s) { (void)s; log_init(); LOG("gbm_surface_lock_front_buffer -> NULL"); return NULL; }
void gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *b) { (void)s;(void)b; log_init(); LOG("gbm_surface_release_buffer"); }

/* ---- misc -------------------------------------------------------------------------------- */
uint32_t gbm_format_canonicalize(uint32_t fmt) { return fmt; }
struct gbm_format_name_desc { char name[5]; };
const char *gbm_format_get_name(uint32_t fmt, struct gbm_format_name_desc *desc) {
    if (desc) {
        desc->name[0] = (char)(fmt & 0xff); desc->name[1] = (char)((fmt >> 8) & 0xff);
        desc->name[2] = (char)((fmt >> 16) & 0xff); desc->name[3] = (char)((fmt >> 24) & 0xff);
        desc->name[4] = 0;
        return desc->name;
    }
    return "?";
}

/* ---- load banner ------------------------------------------------------------------------- */
__attribute__((constructor)) static void probe_load(void) {
    log_init();
    Dl_info self;
    if (dladdr((void *)&probe_load, &self) && self.dli_fname) g_selfpath = self.dli_fname;
    int cf = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
    if (cf >= 0) { ssize_t r = read(cf, g_comm, sizeof(g_comm) - 1); if (r > 0) g_comm[r] = 0; close(cf); }
    for (char *q = g_comm; *q; q++) if (*q == '\n') *q = 0;
    proc_role();
    LOG("LOAD self=%s mode=%s", g_selfpath, g_mode ? "stub" : "nulldev");
}
