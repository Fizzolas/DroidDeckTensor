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

struct gbm_bo *gbm_bo_create(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags) {
    (void)d; (void)flags;
    load_ahb();
    if (!g_ahb_alloc) { errno = ENODEV; return NULL; }
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
    if (rc != 0 || !ahb) { free(b); errno = ENOMEM; return NULL; }
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
        const int *nh = g_ahb_nh(bo->ahb);
        if (!nh || nh[1] < 1) { errno = EINVAL; return -1; }
        bo->fd = fcntl(nh[3], F_DUPFD_CLOEXEC, 0);
    }
    return bo->fd < 0 ? -1 : fcntl(bo->fd, F_DUPFD_CLOEXEC, 0);
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
    if (!bo || !g_ahb_lock) return NULL;
    struct ahb_desc desc = {0};
    desc.width = w ? w : bo->w; desc.height = h ? h : bo->h;
    void *addr = NULL;
    if (g_ahb_lock(bo->ahb, AHB_USAGE, -1, &desc, &addr) != 0) return NULL;
    bo->map = addr;
    if (stride) *stride = bo->stride;
    if (map_data) *map_data = addr;
    return addr;
}
void *gbm_bo_map2(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t flags, uint32_t *stride, void **map_data, int plane) { (void)plane; return gbm_bo_map(bo, x, y, w, h, flags, stride, map_data); }
void gbm_bo_unmap(struct gbm_bo *bo, void *map_data) { (void)map_data; if (bo && bo->map && g_ahb_unlock) { int fence = -1; g_ahb_unlock(bo->ahb, &fence); bo->map = NULL; } }
int gbm_bo_write(struct gbm_bo *bo, const void *buf, size_t count) { (void)bo; (void)buf; (void)count; return 0; }
void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy)(struct gbm_bo *, void *)) { (void)bo; (void)data; (void)destroy; }
void *gbm_bo_get_user_data(struct gbm_bo *bo) { (void)bo; return NULL; }
struct gbm_bo *gbm_bo_new(struct gbm_device *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t flags, uint32_t *mods, int nmods) { (void)mods; (void)nmods; return gbm_bo_create(d, w, h, fmt, flags); }
struct gbm_bo *gbm_bo_import(struct gbm_device *d, uint32_t type, void *buffer, uint32_t usage) { (void)d; (void)type; (void)buffer; (void)usage; return NULL; }

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
