/*
 * A libgbm.so.1 replacement backed by Android's gralloc (AHardwareBuffer).
 *
 * Chromium's GPU process (CEF) allocates its shared-image buffers through GBM. The PowerVR render
 * node has no GBM/GEM, so gbm_bo_get_fd() fails and Chromium's GpuMemoryBufferHandle comes back
 * empty ("Buffer handle is null") and its GPU process aborts. This shim answers the GBM API with a
 * REAL GPU buffer: it reaches bionic's libnativewindow.so (AHardwareBuffer_*) through libhybris's
 * emulated android linker - the same glibc->bionic bridge the Vulkan ICD uses - so the buffer
 * Chromium gets is genuine gralloc memory with a real dma-buf fd, not a placeholder.
 *
 * It is a drop-in: name it libgbm.so.1 and put it in the client's LD_LIBRARY_PATH (BL_CEF_LIBDIR).
 * libhybris.so (and q.so) must sit beside it; the linker dir comes from HYBRIS_LINKER_DIR or the
 * directory this library is in.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

/* ---- libhybris bridge ------------------------------------------------------------------- */
typedef void *(*adlopen_ext_fn)(const char *, int, const void *);
typedef void *(*adlsym_fn)(void *, const char *);

static void *g_hybris;
static adlopen_ext_fn g_adl;
static adlsym_fn g_ads;

/* ---- AHardwareBuffer (libnativewindow) -------------------------------------------------- */
struct ahb_desc { uint32_t width, height, layers, format, usage, stride, rfu0; uint64_t rfu1; };
typedef int  (*ahb_alloc_fn)(const struct ahb_desc *, void **);
typedef void (*ahb_desc_fn)(const void *, struct ahb_desc *);
typedef const int *(*ahb_nh_fn)(const void *);
typedef int  (*ahb_lock_fn)(void *, uint64_t, uint32_t, const struct ahb_desc *, void **);
typedef int  (*ahb_unlock_fn)(void *, int *);
typedef void (*ahb_rel_fn)(void *);
typedef void (*ahb_acq_fn)(void *);

static ahb_alloc_fn g_ahb_alloc;
static ahb_desc_fn  g_ahb_desc;
static ahb_nh_fn    g_ahb_nh;
static ahb_lock_fn  g_ahb_lock;
static ahb_unlock_fn g_ahb_unlock;
static ahb_rel_fn   g_ahb_rel;
static ahb_acq_fn   g_ahb_acq;

static int g_init;

static void load_ahb(void) {
    if (g_init) return;
    g_init = 1;
    const char *dir = getenv("HYBRIS_LINKER_DIR");
    /* The ICD sets HYBRIS_LINKER_DIR; if absent, fall back to this library's own directory. */
    char self[512] = {0};
    if (!dir) {
        Dl_info info;
        if (dladdr((void *)&load_ahb, &info) && info.dli_fname) {
            strncpy(self, info.dli_fname, sizeof(self) - 1);
            char *slash = strrchr(self, '/');
            if (slash) { *slash = 0; dir = self; }
        }
    }
    g_hybris = dlopen("/tmp/libhybris.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_hybris) g_hybris = dlopen("libhybris.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_hybris) { fprintf(stderr, "gbma: no libhybris.so: %s\n", dlerror()); return; }
    g_adl = (adlopen_ext_fn)dlsym(g_hybris, "android_dlopen_ext");
    g_ads = (adlsym_fn)dlsym(g_hybris, "android_dlsym");
    if (!g_adl || !g_ads) { fprintf(stderr, "gbma: libhybris missing android_dlopen_ext/dlsym\n"); return; }
    void *nw = g_adl("/system/lib64/libnativewindow.so", 2, NULL);
    if (!nw) nw = g_adl("libnativewindow.so", 2, NULL);
    if (!nw) { fprintf(stderr, "gbma: libnativewindow.so did not load\n"); return; }
    g_ahb_alloc  = (ahb_alloc_fn) g_ads(nw, "AHardwareBuffer_allocate");
    g_ahb_desc   = (ahb_desc_fn)  g_ads(nw, "AHardwareBuffer_describe");
    g_ahb_nh     = (ahb_nh_fn)    g_ads(nw, "AHardwareBuffer_getNativeHandle");
    g_ahb_lock   = (ahb_lock_fn)  g_ads(nw, "AHardwareBuffer_lock");
    g_ahb_unlock = (ahb_unlock_fn)g_ads(nw, "AHardwareBuffer_unlock");
    g_ahb_rel    = (ahb_rel_fn)   g_ads(nw, "AHardwareBuffer_release");
    g_ahb_acq    = (ahb_acq_fn)   g_ads(nw, "AHardwareBuffer_acquire");
    fprintf(stderr, "gbma: libhybris=%p nw=%p alloc=%p fd=%p\n", g_hybris, nw,
            (void *)g_ahb_alloc, (void *)g_ahb_nh);
}

/* ---- DRM fourcc <-> Android pixel format ------------------------------------------------ */
#define FOURCC(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define DRM_ARGB8888 FOURCC('A','R','2','4')
#define DRM_XRGB8888 FOURCC('X','R','2','4')
#define DRM_ABGR8888 FOURCC('A','B','2','4')
#define DRM_XBGR8888 FOURCC('X','B','2','4')
/* Android PixelFormat (system/graphics.h). */
#define AH_PF_RGBA8888 1
#define AH_PF_RGBX8888 2
#define AH_PF_RGB888   3
#define AH_PF_BGRA8888 5
#define AH_PF_RGBA_F16 22

static uint32_t ahb_format_for(uint32_t drm) {
    switch (drm) {
        case DRM_XRGB8888: return AH_PF_RGBX8888;
        case DRM_ABGR8888: return AH_PF_BGRA8888;   /* ABGR fourcc -> little-endian BGRA bytes */
        case DRM_XBGR8888: return AH_PF_BGRA8888;   /* XBGR ~ BGRA with X in alpha; RGBA path also ok */
        case DRM_ARGB8888:
        default:           return AH_PF_RGBA8888;
    }
}

/* GPU_SAMPLED_IMAGE(0x100)|GPU_FRAMEBUFFER(0x200)|CPU_READ_OFTEN(0x10)|CPU_WRITE_OFTEN(0x20) */
#define AHB_USAGE (0x100u | 0x200u | 0x10u | 0x20u)

/* ---- GBM objects ------------------------------------------------------------------------ */
struct gbm_device { int fd; };
struct gbm_bo {
    void *ahb;          /* AHardwareBuffer* */
    int fd;             /* duplicated dma-buf fd (-1 until get_fd) */
    uint32_t w, h, fmt, stride;
    void *map;          /* AHardwareBuffer_lock base */
    void *map_data;
};
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

static uint32_t stride_of(uint32_t w, uint32_t fmt) {
    uint32_t bpp = 4;
    if (fmt == DRM_XRGB8888 || fmt == DRM_ARGB8888 || fmt == DRM_ABGR8888 || fmt == DRM_XBGR8888) bpp = 4;
    return w * bpp;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    (void)d; (void)flags;
    load_ahb();
    if (!g_ahb_alloc) { errno = ENODEV; return NULL; }
    struct gbm_bo *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fd = -1; b->w = w; b->h = h; b->fmt = fmt; b->stride = stride_of(w, fmt);
    struct ahb_desc desc = {0};
    desc.width = w; desc.height = h; desc.layers = 1;
    desc.format = ahb_format_for(fmt); desc.usage = AHB_USAGE;
    void *ahb = NULL;
    int rc = g_ahb_alloc(&desc, &ahb);
    if (rc != 0 || !ahb) { { int lf = open("/tmp/gbma_bo.log", O_WRONLY|O_APPEND|O_CREAT, 0600); if(lf>=0){char lb[96];int m=snprintf(lb,sizeof(lb),"bo_create FAIL w=%u h=%u fmt=0x%x rc=%d
",w,h,fmt,rc);if(m>0){ssize_t ww=write(lf,lb,m);(void)ww;}close(lf);} } free(b); errno = ENOMEM; return NULL; }
    { int lf = open("/tmp/gbma_bo.log", O_WRONLY|O_APPEND|O_CREAT, 0600);
      if (lf >= 0) { char lb[128]; int m = snprintf(lb,sizeof(lb),"bo_create w=%u h=%u fmt=0x%x -> rc=%d ahb=%p
", w, h, fmt, rc, ahb); if(m>0){ssize_t ww=write(lf,lb,m);(void)ww;} close(lf); } }
    b->ahb = ahb;
    if (g_ahb_desc) { struct ahb_desc o = {0}; g_ahb_desc(ahb, &o); if (o.stride) b->stride = o.stride; }
    return b;
}
struct gbm_bo *gbm_bo_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n) { (void)m; (void)n; return gbm_bo_create(d, w, h, fmt, 0); }
struct gbm_bo *gbm_bo_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n, uint32_t f) { (void)m; (void)n; (void)f; return gbm_bo_create(d, w, h, fmt, 0); }

void gbm_bo_destroy(struct gbm_bo *bo) {
    if (!bo) return;
    if (bo->map && g_ahb_unlock) { int fence = -1; g_ahb_unlock(bo->ahb, &fence); }
    if (bo->fd >= 0) close(bo->fd);
    if (bo->ahb && g_ahb_rel) g_ahb_rel(bo->ahb);
    free(bo);
}

int gbm_bo_get_fd(struct gbm_bo *bo) {
    if (!bo || !bo->ahb || !g_ahb_nh) return -1;
    if (bo->fd < 0) {
        const int *nh = g_ahb_nh(bo->ahb);   /* native_handle: [version, numFds, numInts, fds..., ints...] */
        if (!nh || nh[1] < 1) { errno = EINVAL; return -1; }
        bo->fd = fcntl(nh[3], F_DUPFD_CLOEXEC, 0);
    }
    return bo->fd < 0 ? -1 : fcntl(bo->fd, F_DUPFD_CLOEXEC, 0);
}
int gbm_bo_get_fd_for_plane(struct gbm_bo *bo, int plane) { (void)plane; return gbm_bo_get_fd(bo); }
uint32_t gbm_bo_get_stride(struct gbm_bo *bo) { return bo ? bo->stride : 0; }
uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int p) { (void)p; return bo ? bo->stride : 0; }
uint32_t gbm_bo_get_format(struct gbm_bo *bo) { return bo ? bo->fmt : 0; }
uint32_t gbm_bo_get_width(struct gbm_bo *bo) { return bo ? bo->w : 0; }
uint32_t gbm_bo_get_height(struct gbm_bo *bo) { return bo ? bo->h : 0; }
uint32_t gbm_bo_get_bpp(struct gbm_bo *bo) { (void)bo; return 32; }
int gbm_bo_get_plane_count(struct gbm_bo *bo) { (void)bo; return 1; }
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int p) { (void)bo; (void)p; return 0; }
uint32_t gbm_bo_get_modifier(struct gbm_bo *bo) { (void)bo; return 0; /* LINEAR */ }
void *gbm_bo_get_handle(struct gbm_bo *bo) { return bo ? (void *)(intptr_t)gbm_bo_get_fd(bo) : NULL; }
void *gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int p) { (void)p; return gbm_bo_get_handle(bo); }

void *gbm_bo_map(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data) {
    (void)x; (void)y; (void)flags;
    if (!bo || !g_ahb_lock) return NULL;
    struct ahb_desc desc = {0};
    desc.width = w ? w : bo->w; desc.height = h ? h : bo->h;
    void *addr = NULL;
    int rc = g_ahb_lock(bo->ahb, AHB_USAGE, -1, &desc, &addr);
    if (rc != 0) return NULL;
    bo->map = addr; bo->map_data = addr;
    if (stride) *stride = bo->stride;
    if (map_data) *map_data = addr;
    return addr;
}
void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) {
    (void)map_data;
    if (bo && bo->map && g_ahb_unlock) { int fence = -1; g_ahb_unlock(bo->ahb, &fence); bo->map = NULL; }
}
int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count) { (void)bo; (void)buf; (void)count; return 0; }
void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy)(struct gbm_bo *, void *)) { (void)bo; (void)data; (void)destroy; }
void *gbm_bo_get_user_data(struct gbm_bo *bo) { (void)bo; return NULL; }

/* Surfaces are not used by the ozone/X11 path; keep them minimal. */
struct gbm_surface *gbm_surface_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) { (void)d;(void)w;(void)h;(void)fmt;(void)flags; return NULL; }
struct gbm_surface *gbm_surface_create_with_modifiers(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n) { (void)d;(void)w;(void)h;(void)fmt;(void)m;(void)n; return NULL; }
struct gbm_surface *gbm_surface_create_with_modifiers2(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, const uint64_t *m, unsigned n, uint32_t f) { (void)d;(void)w;(void)h;(void)fmt;(void)m;(void)n;(void)f; return NULL; }
void gbm_surface_destroy(struct gbm_surface *s) { (void)s; }
int gbm_surface_has_free_buffers(struct gbm_surface *s) { (void)s; return 0; }
struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *s) { (void)s; return NULL; }
void gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *bo) { (void)s;(void)bo; }
struct gbm_bo *gbm_bo_import(struct gbm_device *d, uint32_t type, void *buffer, uint32_t usage) { (void)d;(void)type;(void)buffer;(void)usage; return NULL; }

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

/* Load marker: prove the shim is in a process, and whether the AHB bridge initialised. */
__attribute__((constructor)) static void gbma_marker(void) {
    int f = open("/tmp/gbma_loaded.log", O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (f < 0) return;
    char c[64] = {0};
    int cf = open("/proc/self/comm", O_RDONLY);
    if (cf >= 0) { ssize_t n = read(cf, c, sizeof(c) - 1); (void)n; close(cf); }
    for (char *q = c; *q; q++) if (*q == 10) { *q = 0; break; }
    char b[128]; int m = snprintf(b, sizeof(b), "gbma loaded comm=%s pid=%d\n", c, (int)getpid());
    if (m > 0) { ssize_t w = write(f, b, (size_t)m); (void)w; }
    close(f);
    load_ahb();
}

/* Extra symbols Steam's libminigbm.so exports, so a caller that needs the full set resolves. */
void *gbm_bo_get_device(struct gbm_bo *bo) { (void)bo; return NULL; }
int gbm_bo_get_plane_fd(struct gbm_bo *bo, int plane) { (void)plane; return gbm_bo_get_fd(bo); }
size_t gbm_bo_get_plane_size(struct gbm_bo *bo, int plane) { (void)plane; return bo ? (size_t)bo->stride * bo->h : 0; }
void *gbm_bo_map2(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data, int plane) { (void)plane; return gbm_bo_map(bo, x, y, w, h, flags, stride, map_data); }
struct gbm_bo *gbm_bo_new(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t *mods, int nmods) { (void)mods; (void)nmods; return gbm_bo_create(d, w, h, fmt, flags); }
uint32_t gbm_convert_usage(uint64_t usage) { (void)usage; return 0; }
uint32_t gbm_format_canonicalize(uint32_t fmt) { return fmt; }
int gbm_helpers(int x) { (void)x; return 0; }
