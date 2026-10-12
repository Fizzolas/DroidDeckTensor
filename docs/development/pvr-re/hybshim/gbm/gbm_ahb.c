/*
 * A libminigbm.so / libgbm.so.1 replacement backed by Android's gralloc (AHardwareBuffer).
 *
 * Chromium's GPU process (CEF) allocates its shared-image buffers through GBM. The PowerVR render
 * node has no GBM/GEM, so gbm_bo_get_fd() fails, Chromium's GpuMemoryBufferHandle comes back empty
 * ("Buffer handle is null") and its GPU process aborts. This shim answers the GBM API with a REAL
 * GPU buffer: it reaches bionic's libnativewindow.so (AHardwareBuffer_*) through libhybris's
 * emulated android linker - the same glibc->bionic bridge the Vulkan ICD uses.
 *
 * Install as steamrtarm64/libminigbm.so (Steam ships its own minigbm; libgbm.so.1 is not consulted),
 * padded to the real file's size (Steam verifies file sizes). libhybris.so + q.so beside it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

typedef void *(*adlopen_ext_fn)(const char *, int, const void *);
typedef void *(*adlsym_fn)(void *, const char *);

static void *g_hybris;
static adlopen_ext_fn g_adl;
static adlsym_fn g_ads;

struct ahb_desc { uint32_t width, height, layers, format, usage, stride, rfu0; uint64_t rfu1; };
typedef int  (*ahb_alloc_fn)(const struct ahb_desc *, void **);
typedef void (*ahb_desc_fn)(const void *, struct ahb_desc *);
typedef const int *(*ahb_nh_fn)(const void *);
typedef int  (*ahb_lock_fn)(void *, uint64_t, uint32_t, const struct ahb_desc *, void **);
typedef int  (*ahb_unlock_fn)(void *, int *);
typedef void (*ahb_rel_fn)(void *);

static ahb_alloc_fn g_ahb_alloc;
static ahb_desc_fn  g_ahb_desc;
static ahb_nh_fn    g_ahb_nh;
static ahb_lock_fn  g_ahb_lock;
static ahb_unlock_fn g_ahb_unlock;
static ahb_rel_fn   g_ahb_rel;
static int g_init;

static void load_ahb(void) {
    if (g_init) return;
    g_init = 1;
    g_hybris = dlopen("/tmp/libhybris.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_hybris) g_hybris = dlopen("libhybris.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_hybris) { fprintf(stderr, "gbma: no libhybris.so: %s\n", dlerror()); return; }
    g_adl = (adlopen_ext_fn)dlsym(g_hybris, "android_dlopen_ext");
    g_ads = (adlsym_fn)dlsym(g_hybris, "android_dlsym");
    if (!g_adl || !g_ads) { fprintf(stderr, "gbma: libhybris missing symbols\n"); return; }
    void *nw = g_adl("/system/lib64/libnativewindow.so", 2, NULL);
    if (!nw) nw = g_adl("libnativewindow.so", 2, NULL);
    if (!nw) { fprintf(stderr, "gbma: libnativewindow.so did not load\n"); return; }
    g_ahb_alloc  = (ahb_alloc_fn) g_ads(nw, "AHardwareBuffer_allocate");
    g_ahb_desc   = (ahb_desc_fn)  g_ads(nw, "AHardwareBuffer_describe");
    g_ahb_nh     = (ahb_nh_fn)    g_ads(nw, "AHardwareBuffer_getNativeHandle");
    g_ahb_lock   = (ahb_lock_fn)  g_ads(nw, "AHardwareBuffer_lock");
    g_ahb_unlock = (ahb_unlock_fn)g_ads(nw, "AHardwareBuffer_unlock");
    g_ahb_rel    = (ahb_rel_fn)   g_ads(nw, "AHardwareBuffer_release");
    fprintf(stderr, "gbma: hybris=%p nw=%p alloc=%p fd=%p\n", g_hybris, nw,
            (void *)g_ahb_alloc, (void *)g_ahb_nh);
}

#define FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define DRM_ARGB8888 FOURCC('A','R','2','4')
#define DRM_XRGB8888 FOURCC('X','R','2','4')
#define DRM_ABGR8888 FOURCC('A','B','2','4')
#define DRM_XBGR8888 FOURCC('X','B','2','4')
#define AH_PF_RGBA8888 1
#define AH_PF_RGBX8888 2
#define AH_PF_BGRA8888 5
#define AHB_USAGE (0x100u | 0x200u | 0x10u | 0x20u)

static uint32_t ahb_format_for(uint32_t drm) {
    switch (drm) {
        case DRM_XRGB8888: return AH_PF_RGBX8888;
        case DRM_ABGR8888:
        case DRM_XBGR8888: return AH_PF_BGRA8888;
        case DRM_ARGB8888:
        default:           return AH_PF_RGBA8888;
    }
}

struct gbm_device { int fd; };
struct gbm_bo { void *ahb; int fd; uint32_t w, h, fmt, stride; void *map; };
struct gbm_surface { int unused; };

struct gbm_device *gbm_create_device(int fd) {
    load_ahb();
    struct gbm_device *d = calloc(1, sizeof(*d));
    if (d) d->fd = fd;
    return d;
}
void gbm_device_destroy(struct gbm_device *d) { free(d); }
const char *gbm_device_get_backend_name(struct gbm_device *d) { (void)d; return "droiddeck-ahb"; }
int gbm_device_get_fd(struct gbm_device *d) { return d->fd; }
int gbm_device_is_format_supported(struct gbm_device *d, uint32_t fmt, uint32_t usage) { (void)d; (void)fmt; (void)usage; return 1; }
uint32_t gbm_device_get_format_modifier_plane_count(struct gbm_device *d, uint32_t fmt, uint64_t mod) { (void)d; (void)fmt; (void)mod; return 1; }

static void logline(const char *fmt, ...) {
    int f = open("/root/.local/share/Steam/logs/gbma.log", O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (f < 0) return;
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0) { ssize_t w = write(f, b, (size_t)n); (void)w; }
    close(f);
}

struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    (void)d; (void)flags;
    load_ahb();
    if (!g_ahb_alloc) { logline("gbm_bo_create FAIL: no AHB alloc (hybris not loaded)\n"); errno = ENODEV; return NULL; }
    struct gbm_bo *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fd = -1; b->w = w; b->h = h; b->fmt = fmt; b->stride = w * 4;
    struct ahb_desc desc = {0};
    desc.width = w; desc.height = h; desc.layers = 1;
    desc.format = ahb_format_for(fmt); desc.usage = AHB_USAGE;
    void *ahb = NULL;
    int rc = g_ahb_alloc(&desc, &ahb);
    if (rc != 0 || !ahb) {
        struct ahb_desc d2 = desc; d2.format = AH_PF_RGBA8888; d2.usage = 0x100u | 0x200u;
        rc = g_ahb_alloc(&d2, &ahb);
    }
    if (rc != 0 || !ahb) { logline("gbm_bo_create w=%u h=%u fmt=0x%x -> FAIL rc=%d\n", w, h, fmt, rc); free(b); errno = ENOMEM; return NULL; }
    logline("gbm_bo_create w=%u h=%u fmt=0x%x -> ok ahb=%p\n", w, h, fmt, ahb);
    b->ahb = ahb;
    if (g_ahb_desc) { struct ahb_desc o = {0}; g_ahb_desc(ahb, &o); if (o.stride) b->stride = o.stride; }
    return b;
}
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n) { (void)m; (void)n; return gbm_bo_create(d, w, h, fmt, 0); }
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n, uint32_t f) { (void)m; (void)n; (void)f; return gbm_bo_create(d, w, h, fmt, 0); }

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo) return;
    logline("gbm_bo_destroy %p\n", (void*)bo);
    if (bo->map && g_ahb_unlock) { int fence = -1; g_ahb_unlock(bo->ahb, &fence); }
    if (bo->fd >= 0) close(bo->fd);
    if (bo->ahb && g_ahb_rel) g_ahb_rel(bo->ahb);
    free(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    if (!bo || !bo->ahb || !g_ahb_nh) return -1;
    if (bo->fd < 0) {
        const int *nh = g_ahb_nh(bo->ahb);
        if (!nh || nh[1] < 1) { logline("gbm_bo_get_fd NULL handle\n"); errno = EINVAL; return -1; }
        bo->fd = fcntl(nh[3], F_DUPFD_CLOEXEC, 0);
    }
    int r = bo->fd < 0 ? -1 : fcntl(bo->fd, F_DUPFD_CLOEXEC, 0);
    logline("gbm_bo_get_fd -> %d (w=%u h=%u fmt=0x%x)\n", r, bo->w, bo->h, bo->fmt);
    return r;
}
int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) { (void)plane; return gbm_bo_get_fd(bo); }
int gbm_bo_get_plane_fd(struct gbm_bo *bo, int plane) { (void)plane; return gbm_bo_get_fd(bo); }
size_t gbm_bo_get_plane_size(struct gbm_bo *bo, int plane) { (void)plane; return bo ? (size_t)bo->stride * bo->h : 0; }
uint32_t gbm_bo_get_stride(struct gbm_bo *bo) { return bo ? bo->stride : 0; }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int p) { (void)p; return bo ? bo->stride : 0; }
uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo ? bo->fmt : 0; }
uint32_t gbm_bo_get_width(struct gbm_bo *bo) { return bo ? bo->w : 0; }
uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo ? bo->h : 0; }
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) { (void)bo; return 32; }
int gbm_bo_get_plane_count(struct gbm_bo *bo) { (void)bo; return 1; }
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int p) { (void)bo; (void)p; return 0; }
uint32_t gbm_bo_get_modifier(struct gbm_bo *bo) { (void)bo; return 0; }
void *gbm_bo_get_device(struct gbm_bo *bo) { (void)bo; return NULL; }
void *gbm_bo_get_handle(struct gbm_bo *bo) { return bo ? (void *)(intptr_t)gbm_bo_get_fd(bo) : NULL; }
void *gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int p) { (void)p; return gbm_bo_get_handle(bo); }

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data) {
    (void)x; (void)y; (void)flags;
    logline("gbm_bo_map %p w=%u h=%u\n", (void*)bo, bo ? bo->w : 0, bo ? bo->h : 0);
    if (!bo || !g_ahb_lock) return NULL;
    /* AHardwareBuffer_lock's 4th argument is an ARect {left,top,right,bottom}, not a desc - passing a
     * desc here made the lock fail (map returned NULL, which CEF then dereferenced). NULL locks the
     * whole buffer, which is what a GBM map of the whole bo wants. */
    /* Lock usage must be a CPU access flag - the GPU bits used for allocation are not valid for
     * AHardwareBuffer_lock. The rect is {left,top,right,bottom}; use the whole buffer. */
    int rect[4] = {0, 0, (int)bo->w, (int)bo->h};
    void *addr = NULL;
    int lrc = g_ahb_lock(bo->ahb, 0x10u | 0x20u, -1, rect, &addr);
    if (lrc != 0 || !addr) {
        logline("gbm_bo_map FAIL: AHB lock rc=%d addr=%p\n", lrc, addr);
        return NULL;
    }
    bo->map = addr;
    if (stride) *stride = bo->stride;
    if (map_data) *map_data = addr;
    logline("gbm_bo_map -> addr=%p stride=%u\n", addr, bo->stride);
    return addr;
}
void *gbm_bo_map2(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data, int plane) { (void)plane; return gbm_bo_map(bo, x, y, w, h, flags, stride, map_data); }
void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) { (void)map_data; if (bo && bo->map && g_ahb_unlock) { int fence = -1; g_ahb_unlock(bo->ahb, &fence); bo->map = NULL; } }
int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count) { (void)bo; (void)buf; (void)count; return 0; }
void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy)(struct gbm_bo *, void *)) { (void)bo; (void)data; (void)destroy; }
void *gbm_bo_get_user_data(struct gbm_bo *bo) { (void)bo; return NULL; }
struct gbm_bo *gbm_bo_new(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t *mods, int nmods) { (void)mods; (void)nmods; return gbm_bo_create(d, w, h, fmt, flags); }
/* gbm_bo_import: CEF's GPU process imports a dma-buf (or ANativeWindowBuffer) as a bo - this is the
 * call that produces the "Buffer handle" it needs. type is GBM_BO_IMPORT_FD(2),
 * GBM_BO_IMPORT_FD_MODIFIER(4) or GBM_BO_IMPORT_WL_BUFFER(0).
 *   GBM_BO_IMPORT_FD:         buffer = struct gbm_import_fd_data { int fd; u32 width, height, stride; u32 format; }
 *   GBM_BO_IMPORT_FD_MODIFIER: buffer = struct gbm_import_fd_modifier_data { u32 width,height,format; u32 num_fds;
 *                                    int fds[4]; int strides[4]; int offsets[4]; u64 modifier; }
 * We keep the imported fd and answer get_fd/stride/format from it, so a caller round-tripping an fd
 * through import -> get_fd gets a real buffer. */
struct gbm_import_fd_data { int fd; uint32_t width, height, stride, format; };
struct gbm_import_fd_modifier_data { uint32_t width, height, format, num_fds; int fds[4]; int strides[4]; int offsets[4]; uint64_t modifier; };

static int ahb_import_fd(int fd, uint32_t w, uint32_t h, uint32_t fmt, void **out_ahb) {
    /* Newer Android exposes AHardwareBuffer_createFromHandle(desc, handle, usage, out). Build a
     * native_handle from the fd and call it if available. */
    (void)w; (void)h; (void)fmt; (void)out_ahb; (void)fd;
    return -1;   /* not available through the libnativewindow the shim holds yet */
}

struct gbm_bo *gbm_bo_import(struct gbm_device *d, uint32_t type, void *buffer, uint32_t usage) {
    (void)d; (void)usage;
    load_ahb();
    struct gbm_bo *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fd = -1;
    if (type == 2 /* GBM_BO_IMPORT_FD */ && buffer) {
        struct gbm_import_fd_data *im = buffer;
        b->w = im->width; b->h = im->height; b->fmt = im->format; b->stride = im->stride;
        b->fd = fcntl(im->fd, F_DUPFD_CLOEXEC, 0);
    } else if (type == 4 /* GBM_BO_IMPORT_FD_MODIFIER */ && buffer) {
        struct gbm_import_fd_modifier_data *im = buffer;
        b->w = im->width; b->h = im->height; b->fmt = im->format; b->stride = im->strides[0];
        if (im->num_fds >= 1) b->fd = fcntl(im->fds[0], F_DUPFD_CLOEXEC, 0);
    } else {
        logline("gbm_bo_import type=%u unsupported\n", type);
        free(b);
        return NULL;
    }
    logline("gbm_bo_import type=%u w=%u h=%u fmt=0x%x fd=%d\n", type, b->w, b->h, b->fmt, b->fd);
    return b;
}

struct gbm_surface *gbm_surface_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) { (void)d; (void)w; (void)h; (void)fmt; (void)flags; return NULL; }
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n) { (void)d; (void)w; (void)h; (void)fmt; (void)m; (void)n; return NULL; }
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n, uint32_t f) { (void)d; (void)w; (void)h; (void)fmt; (void)m; (void)n; (void)f; return NULL; }
void gbm_surface_destroy(struct gbm_surface *s) { (void)s; }
int gbm_surface_has_free_buffers(struct gbm_surface *s) { (void)s; return 0; }
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s) { (void)s; return NULL; }
void gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *bo) { (void)s; (void)bo; }

uint32_t gbm_convert_usage(uint64_t usage) { (void)usage; return 0; }
uint32_t gbm_format_canonicalize(uint32_t fmt) { return fmt; }
int gbm_helpers(int x) { (void)x; return 0; }

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

__attribute__((constructor)) static void gbma_load_marker(void) {
    int f = open("/root/.local/share/Steam/logs/gbma_loaded.log", O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (f < 0) return;
    char c[64] = {0}; int cf = open("/proc/self/comm", O_RDONLY);
    if (cf >= 0) { ssize_t n = read(cf, c, sizeof(c) - 1); (void)n; close(cf); }
    for (char *q = c; *q; q++) if (*q == 10) { *q = 0; break; }
    char b[128]; int m = snprintf(b, sizeof(b), "gbma loaded comm=%s pid=%d\n", c, (int)getpid());
    if (m > 0) { ssize_t w = write(f, b, (size_t)m); (void)w; }
    close(f);
}

/* Crash capture: the shim is loaded by libcef.so in the GPU process, so this constructor runs there.
 * Install a fatal-signal handler that records the crash pc/lr/si_addr to the shim log, so a crash
 * that Chromium reports only as "GPU process exited" can be located. */
#include <signal.h>
#include <ucontext.h>
static void gbma_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = (ucontext_t *)ctx;
    char b[256];
    int n = snprintf(b, sizeof(b), "CRASH sig=%d si_addr=%p pc=%p lr=%p sp=%p\n",
                     sig, si ? si->si_addr : 0, uc ? (void *)uc->uc_mcontext.pc : 0,
                     uc ? (void *)uc->uc_mcontext.regs[30] : 0,
                     uc ? (void *)uc->uc_mcontext.sp : 0);
    int f = open("/root/.local/share/Steam/logs/gbma_crash.log", O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (f >= 0) { if (n > 0) { ssize_t w = write(f, b, (size_t)n); (void)w; } close(f); }
    _exit(139);
}
__attribute__((constructor)) static void gbma_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = gbma_crash;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
