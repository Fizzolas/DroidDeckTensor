#define _GNU_SOURCE 1
/*
 * pvr_icd.c — a glibc Vulkan ICD that drives the bionic PowerVR Vulkan HAL through libhybris.
 *
 * Built as `libvulkan_powervr.so`; a glibc Vulkan loader discovers it by a manifest whose
 * library_path points here (VK_ICD_FILENAMES / VK_DRIVER_FILES). It:
 *   1. is linked against libhybris.so (DT_NEEDED), so the bionic->glibc hook layer is present;
 *   2. uses libhybris's android_dlopen_ext to load the bionic HAL and its deps;
 *   3. gets HMI (hw_module_t), open("vk0") -> the hwvulkan device, and its GetInstanceProcAddr;
 *   4. forwards the loader's ICD entrypoints straight to the HAL.
 *
 * The HAL is a full Vulkan implementation (1.4.317 on Tensor G5), so this wrapper is thin: it
 * exists only to load the bionic code in a glibc process and re-expose it under the ICD ABI.
 *
 * Env the process must set:
 *   HYBRIS_LINKER_DIR      = dir holding the libhybris linker DSO (q.so)
 *   HYBRIS_LD_LIBRARY_PATH = /system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

/* libhybris entry points (provided by libhybris.so, which this DSO links against). */
extern void *android_dlopen_ext(const char *filename, int flag, const void *info);
extern void *android_dlsym(void *handle, const char *symbol);
extern char *android_dlerror(void);

#ifndef RTLD_NOW
#define RTLD_NOW 2   /* bionic's value; the host includes often define their own */
#endif
#define VK_SUCCESS 0
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_INCOMPLETE 5
#define VK_ERROR_LAYER_NOT_PRESENT (-6)
typedef int32_t VkResult;

typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;

/* Android hw_module_t / hwvulkan device, spelled out (no Android headers). */
typedef struct hw_module_t hw_module_t;
typedef struct { int (*open)(const hw_module_t *, const char *, void **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id, *name, *author; hw_module_methods_t *methods; void *dso; uint32_t reserved[25];
};
typedef struct { uint64_t w[18]; } hwvulkan_device_t; /* CreateInstance @0x80, GIPA @0x88 (from the blob's open()) */

static const char *const BLOB = "/vendor/lib64/hw/vulkan.powervr.so";

static void *g_blob;                 /* the HAL module handle */
static void *(*g_gipa)(VkInstance, const char *);   /* the HAL's vkGetInstanceProcAddr */
static void *(*g_create)(const void *, const void *, VkInstance *);
static VkInstance g_instance;         /* the instance we made for the HAL */

/* The instance extensions this ICD provides on top of the HAL (the HAL has only Android surfaces). */
static const char *const ICD_INSTANCE_EXTENSIONS[] = {
    "VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface" };
#define ICD_NUM_INSTANCE_EXTENSIONS 4

static void icd_log(const char *m) { fprintf(stderr, "pvr_icd: %s\n", m); }

/* Per-call tracing, off unless asked for. gamescope probes a DRM modifier once per output format per
 * frame, so an unconditional line here buried the session log under hundreds of thousands of them and
 * made the guest's own errors hard to find. The fixes themselves stay silent: a retry that succeeds is
 * the expected path now, not news. `PVR_ICD_DEBUG=1` in the guest's environment turns it back on. */
static int icd_debug_on(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("PVR_ICD_DEBUG"); on = (v && *v && *v != '0'); }
    return on;
}

/* The Wayland WSI this ICD carries (the HAL only does Android surfaces). */
extern void *pvr_wsi_proc(const char *name);
/* Told about each device the HAL creates, so the WSI can complete an acquire's fence/semaphore with
 * the HAL's own queue entries (see the note in pvr_wsi.c). */
extern void pvr_wsi_register_device(VkDevice dev, uint32_t queue_family, void *queue_submit,
                                    void *create_fence, void *wait_fences, void *destroy_fence,
                                    void *get_device_queue);
static int icd_load(void);   /* defined below; used by pvr_icd_resolve */

/* Self-configuration: the guest session runs with a clean environment (env -i), so this ICD points
 * the libhybris layer at its own directory (found from this object's path) if the caller has not. */
__attribute__((constructor)) static void pvr_icd_ctor(void) {
    Dl_info info;
    static char dir[4096], linker[4096];
    if (!getenv("HYBRIS_LD_LIBRARY_PATH"))
        setenv("HYBRIS_LD_LIBRARY_PATH",
               "/system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw", 1);
    if (!getenv("HYBRIS_LINKER_DIR") && dladdr((void *)pvr_icd_ctor, &info) && info.dli_fname) {
        snprintf(dir, sizeof(dir), "%s", info.dli_fname);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            snprintf(linker, sizeof(linker), "%s", dir);
            setenv("HYBRIS_LINKER_DIR", linker, 1);
            fprintf(stderr, "pvr_icd: HYBRIS_LINKER_DIR=%s\n", linker);
        }
    }
}

/* Resolve a dynamic Vulkan entrypoint through the HAL. The WSI (its own translation unit) uses this
 * instead of referencing the global vkGetDeviceProcAddr, which an ICD must not have undefined. */
static void *icd_gdpa(VkDevice device, const char *pName);   /* forward */
__attribute__((visibility("default")))
void *pvr_icd_resolve(void *instance, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)icd_gdpa;
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa((VkInstance)instance, name);
}

/* Diagnostic: the HAL's PLT at open, so it can be compared with the same table at a later crash.
 * The question is whether a slot's value CHANGES during the process's life (something rewrites it)
 * or was already wrong when the HAL was opened. Off unless PVR_ICD_DIAG names a file to append to. */
#define HAL_OPEN_VADDR   0x1b9ee0u
#define HAL_GOT_PLT_OFF  0x286ab0u
#define HAL_GOT_PLT_N    349
static FILE *diagf(void) {
    static FILE *f; static int tried;
    if (!tried) { const char *p = getenv("PVR_ICD_DIAG"); tried = 1; if (p) f = fopen(p, "a"); }
    return f;
}
/* Every symbol libhybris is asked to resolve for the HAL, and (through diag_plt) what the HAL's PLT
 * ends up holding. The callback is libhybris's documented seam; setting it here is safe because the
 * ICD loads the HAL, so it is registered before any of the HAL's symbols are resolved. NULL means
 * "resolve it normally". */
typedef void *(*hook_cb_t)(const char *name, const char *requester);
extern void hybris_set_hook_callback(hook_cb_t callback);
static void *diag_hook(const char *name, const char *requester) {
    FILE *o = diagf();
    if (o && name) {
        fprintf(o, "ICD hook pid=%d sym=%s requester=%s\n",
                (int)getpid(), name, requester ? requester : "(null)");
        fflush(o);
    }
    return NULL;
}

static void diag_plt(unsigned long long base, const char *when) {
    FILE *o = diagf();
    if (!o || !base) return;
    char cb[64] = {0};
    int cf = open("/proc/self/comm", O_RDONLY);
    if (cf >= 0) { ssize_t r = read(cf, cb, sizeof(cb) - 1); (void)r; close(cf); }
    for (char *q = cb; *q; q++) if (*q == 10) { *q = 0; break; }
    unsigned long long *g = (unsigned long long *)(uintptr_t)(base + HAL_GOT_PLT_OFF);
    fprintf(o, "ICD %s pid=%d comm=%s base=%llx", when, (int)getpid(), cb, base);
    static const int w[] = {13,14,30,57,68,70,73,75,76,77,167,168,197,198,220,348,349};
    for (unsigned k = 0; k < sizeof(w)/sizeof(w[0]); k++)
        fprintf(o, " %d=%llx", w[k], g[w[k]]);
    fprintf(o, "\n");
    fflush(o);
}

/* Load the bionic HAL once. Returns 0 on success. */
static int icd_load(void) {
    if (g_gipa) return 0;
    if (diagf()) hybris_set_hook_callback(diag_hook);   /* before the HAL's symbols are resolved */
    g_blob = android_dlopen_ext(BLOB, RTLD_NOW, NULL);
    if (!g_blob) { fprintf(stderr, "pvr_icd: android_dlopen_ext(%s) failed: %s\n", BLOB, android_dlerror()); return -1; }

    hw_module_t *mod = (hw_module_t *)android_dlsym(g_blob, "HMI");
    if (!mod || !mod->methods || !mod->methods->open) { icd_log("no HMI/open in the HAL"); return -1; }

    { Dl_info hi; unsigned long long hb = 0;
      if (dladdr((void *)mod->methods->open, &hi) && hi.dli_fbase)
          hb = (unsigned long long)(uintptr_t)hi.dli_fbase;
      /* dladdr cannot see a libhybris-loaded library, so fall back to the known file offset of the
       * entry point (vulkan_device_open is at 0x1b9ee0). */
      if (!hb) hb = (unsigned long long)(uintptr_t)mod->methods->open - HAL_OPEN_VADDR;
      diag_plt(hb, "before_open"); }
    void *dev = NULL;
    int rc = mod->methods->open(mod, "vk0", &dev);
    { unsigned long long hb = (unsigned long long)(uintptr_t)mod->methods->open - HAL_OPEN_VADDR;
      diag_plt(hb, "after_open"); }
    if (rc != 0 || !dev) { fprintf(stderr, "pvr_icd: HAL open(vk0) rc=%d\n", rc); return -1; }

    hwvulkan_device_t *d = (hwvulkan_device_t *)dev;
    g_gipa = (void *(*)(VkInstance, const char *))d->w[0x88 / 8];
    g_create = (void *(*)(const void *, const void *, VkInstance *))d->w[0x80 / 8];
    if (!g_gipa) { icd_log("HAL device has no GetInstanceProcAddr"); return -1; }
    return 0;
}

/* ---- ICD interface ---------------------------------------------------------- */

/* Device entrypoints: ours (the WSI) first, then the HAL's. The loader consults this to decide
 * whether VK_KHR_swapchain is supported, so vkCreateSwapchainKHR/vkQueuePresentKHR must be here. */
/* VkDeviceCreateInfo, spelled out so we can rewrite the extension list in place. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const void *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} DeviceCreateInfo;

/* vkCreateDevice: our WSI provides the swapchain, not the HAL. The HAL's own swapchain is
 * Android-surface based and fails to initialise in a glibc guest, so we must not enable
 * VK_KHR_swapchain on the HAL device -- keep it for the app (our vkCreateSwapchainKHR serves it)
 * and drop it on the way to the HAL. */
static VkResult icd_create_device(void *pd, const void *ci, const void *alloc, void **out) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal)(void *, const void *, const void *, void **) =
        (VkResult (*)(void *, const void *, const void *, void **))g_gipa(g_instance, "vkCreateDevice");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceCreateInfo *in = (const DeviceCreateInfo *)ci;
    uint32_t n = in->enabledExtensionCount;
    const char *const *names = in->ppEnabledExtensionNames;
    /* Our WSI needs the external-memory handles on the HAL device to export swapchain dma-bufs. */
    static const char *const need[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    int have[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 2; k++) if (!strcmp(names[i], need[k])) have[k] = 1;
    const char **kept = (const char **)malloc(sizeof(char *) * (n + 2));
    uint32_t kn = 0;
    for (uint32_t i = 0; i < n; i++) if (strcmp(names[i], "VK_KHR_swapchain")) kept[kn++] = names[i];
    for (int k = 0; k < 2; k++) if (!have[k]) kept[kn++] = need[k];
    DeviceCreateInfo copy = *in;
    copy.enabledExtensionCount = kn;
    copy.ppEnabledExtensionNames = kept;
    VkResult r = hal(pd, &copy, alloc, out);
    free(kept);
    if (r != VK_SUCCESS) return r;
    /* The WSI drives its own queue submissions to complete a frame's fences and semaphores, so it
     * needs the HAL's device entrypoints -- and the queue family the app's queues come from, taken
     * from the first VkDeviceQueueCreateInfo (this device has one general family). */
    if (out && *out) {
        uint32_t qf = 0;
        const uint32_t *q = (const uint32_t *)((const uint32_t *)ci + 3);  /* queueCreateInfoCount */
        if (q && *q > 0) qf = q[1];                                        /* pQueueCreateInfos[0].queueFamilyIndex */
        void *(*gdpa)(void *, const char *) =
            (void *(*)(void *, const char *))g_gipa(*out, "vkGetDeviceProcAddr");
        if (gdpa) {
            pvr_wsi_register_device(*out, qf, gdpa(*out, "vkQueueSubmit"), gdpa(*out, "vkCreateFence"),
                                    gdpa(*out, "vkWaitForFences"), gdpa(*out, "vkDestroyFence"),
                                    gdpa(*out, "vkGetDeviceQueue"));
        }
    }
    return r;
}

static void *icd_gdpa(VkDevice device, const char *pName) {
    void *wsi = pvr_wsi_proc(pName);
    if (wsi) return wsi;
    if (!g_gipa && icd_load() != 0) return NULL;
    void *(*hal_gdpa)(VkDevice, const char *) =
        (void *(*)(VkDevice, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
    return hal_gdpa ? hal_gdpa(device, pName) : NULL;
}

/* Device extensions this ICD adds on top of the HAL: the swapchain the WSI presents to, and the
 * external-memory handles its buffers use. The loader validates requested device extensions against
 * this list, so it must include them. */
static const char *const ICD_DEVICE_EXTENSIONS[] = {
    "VK_KHR_swapchain", "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
};
#define ICD_NUM_DEVICE_EXTENSIONS 4

static int icd_has_our_device_ext(const char *name) {
    for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) if (!strcmp(name, ICD_DEVICE_EXTENSIONS[i])) return 1;
    return 0;
}

/* The loader fetches a physical-device proc address through this and caches what it returns; if it
 * gets the HAL's copy it never sees the WSI extensions. So ours answers for the enumeration. */
static VkResult icd_enum_device_ext(void *pd, const char *layer, uint32_t *count, void *props);
static void *icd_gpdpa(void *pd, const char *pName) {
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void *)icd_enum_device_ext;
    if (!g_gipa && icd_load() != 0) return NULL;
    void *(*hal)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetPhysicalDeviceProcAddr");
    return hal ? hal(pd, pName) : NULL;
}

static VkResult icd_enum_device_ext(void *pd, const char *layer, uint32_t *count, void *props) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*he)(void *, const char *, uint32_t *, void *) =
        (VkResult (*)(void *, const char *, uint32_t *, void *))g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!he) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t hc = 0; he(pd, layer, &hc, NULL);
    /* Only add the ones the HAL does not already report. */
    int add[ICD_NUM_DEVICE_EXTENSIONS], nadd = 0;
    void *tmp = NULL;
    struct { char name[256]; uint32_t ver; } *arr0 = NULL;
    if (hc) { tmp = malloc(hc * 260); he(pd, layer, &hc, tmp); arr0 = tmp;
        for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) {
            int present = 0; for (uint32_t k = 0; k < hc; k++) if (!strcmp(arr0[k].name, ICD_DEVICE_EXTENSIONS[i])) { present = 1; break; }
            if (!present) add[nadd++] = i;
        } }
    else { for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) add[nadd++] = i; }
    if (!props) { *count = hc + nadd; free(tmp); return VK_SUCCESS; }
    uint32_t room = *count;
    uint32_t cap = room < hc ? room : hc;
    VkResult r = he(pd, layer, &cap, props);
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    uint32_t out = cap;
    for (int i = 0; i < nadd && out < room; i++) { strncpy(arr[out].name, ICD_DEVICE_EXTENSIONS[add[i]], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++; }
    *count = out; free(tmp);
    return (r == VK_INCOMPLETE || cap < hc) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Append the extensions this ICD provides to whatever the HAL reports, so the loader lets an app
 * enable VK_KHR_wayland_surface. Returns whether [name] is ours. */
static int icd_is_our_extension(const char *name) {
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS; i++)
        if (!strcmp(name, ICD_INSTANCE_EXTENSIONS[i])) return 1;
    return 0;
}

static VkResult icd_enum_instance_ext(const char *layer, uint32_t *count, void *props) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*he)(const char *, uint32_t *, void *) = (VkResult (*)(const char *, uint32_t *, void *))g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!he) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t hc = 0; he(NULL, &hc, NULL);
    if (!props) { *count = hc + ICD_NUM_INSTANCE_EXTENSIONS; return VK_SUCCESS; }
    uint32_t room = *count;
    uint32_t cap = room < hc ? room : hc;
    VkResult r = he(NULL, &cap, props);
    uint32_t out = cap;
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS && out < room; i++) {
        strncpy(arr[out].name, ICD_INSTANCE_EXTENSIONS[i], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++;
    }
    *count = out;
    return (r == VK_INCOMPLETE || cap < hc) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult icd_enum_instance_ext_OLD(const char *layer, uint32_t *count, void *props) {
    (void)layer; (void)count; (void)props;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal_enum)(const char *, uint32_t *, void *) = (VkResult (*)(const char *, uint32_t *, void *))g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!hal_enum) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    hal_enum(NULL, &n, NULL);
    if (!props) { *count = n + ICD_NUM_INSTANCE_EXTENSIONS; return VK_SUCCESS; }
    uint32_t room = *count;
    uint32_t cap = room < n ? room : n;
    VkResult r = hal_enum(NULL, &cap, props);
    uint32_t out = cap;
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS && out < room; i++) {
        strncpy(arr[out].name, ICD_INSTANCE_EXTENSIONS[i], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++;
    }
    *count = out;
    return (r == VK_INCOMPLETE || cap < n) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- VK_EXT_physical_device_drm -------------------------------------------------------------
 *
 * The PowerVR HAL advertises this extension and then writes NOTHING into
 * VkPhysicalDeviceDrmPropertiesEXT - measured on device: with the struct poisoned, every field is
 * still the poison after vkGetPhysicalDeviceProperties2 returns (see probes/pvr_drm_props_probe.c).
 * An app that zero-initialises the struct (the spec's advice) therefore reads hasPrimary = 0.
 *
 * gamescope 3.16.29 gates its backend on exactly that field (rendervulkan.cpp):
 *     if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary ) { "physical device has no
 *     primary node"; return false; }
 * and CWaylandBackend::UsesVulkanSwapchain() is false, so the nested Wayland backend was required to
 * have a KMS primary node it never opens. With the struct left empty the gate always fired, the
 * backend was deleted, and gamescope fell back to SDL against the instance it already made with an
 * empty extension list, which dies with "VK_KHR_wayland_surface extension is not enabled in the
 * Vulkan instance".
 *
 * The HAL's silence is what has to be corrected, so fill the struct in here from the node the HAL
 * actually drives. This is the answer a working DRM driver would give; the HAL omitting it is the
 * defect, not a fact to pass on. The gate is therefore satisfied truthfully rather than bypassed.
 *
 * hasPrimary comes out true on this device because it IS true: the PowerVR GPU exposes a card node
 * of its own. /sys/class/drm/card2 and /sys/class/drm/renderD128 are both symlinks into
 * /sys/devices/platform/34f00000.gpu0/drm/, and libdrm's drmGetDeviceFromDevId() reports them as one
 * device (probes/pvr_drm_gate_probe.c: "available_nodes: primary=/dev/dri/card2
 * render=/dev/dri/renderD128"). Nothing here invents a node: a device whose kernel driver registers
 * no primary would still report hasPrimary = 0, which is the correct answer for it.
 *
 * The entrypoint is reached through vk_icdGetInstanceProcAddr, which is where the loader resolves it
 * (its own trampoline calls the pointer the ICD returned there). Putting it in
 * vk_icdGetPhysicalDeviceProcAddr instead is dead code: the loader only consults that for entry
 * points it has no trampoline for, and vkGetPhysicalDeviceProperties2 has one. */
#define STYPE_PHYS_DEV_PROPERTIES_2 1000059000u
#define STYPE_DRM_PROPERTIES_EXT    1000353000u

typedef struct {
    uint32_t sType; void *pNext;
    uint32_t hasPrimary, hasRender;
    int64_t primaryMajor, primaryMinor, renderMajor, renderMinor;
} PhysDevDrmProps;
typedef struct { uint32_t sType; void *pNext; void *properties; } PhysDevProps2;

static int g_drm_dev_known;
static int g_drm_has_primary;
static int64_t g_drm_primary_major, g_drm_primary_minor, g_drm_render_major, g_drm_render_minor;

/* Which DRM device a node belongs to, the way libdrm answers it: a card node and a render node are
 * the same device when the kernel put them in one device's sysfs directory, so their
 * /sys/dev/char/<maj>:<min>/device links resolve to the same target. Comparing device NUMBERS would
 * not work - card2 is 226:2 and renderD128 is 226:128, different numbers, one device. This phone's
 * card0 (display) and card1 (2D block) are separate DRM devices with their own directories, so they
 * correctly do not count as this GPU's primary. */
static int icd_node_device(const char *node, int64_t *maj, int64_t *min, char *device, size_t devlen) {
    struct stat st;
    char link[128], path[64];
    if (stat(node, &st) != 0) return 0;
    *maj = major(st.st_rdev); *min = minor(st.st_rdev);
    snprintf(path, sizeof(path), "/sys/dev/char/%lld:%lld/device", (long long)*maj, (long long)*min);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0) return 0;
    link[n] = 0;
    snprintf(device, devlen, "%s", link);
    return 1;
}

/* The primary node of a render node's DRM device: the card* entry of that device's own sysfs
 * device/drm/ directory. Being in that directory IS the membership test - the kernel puts a device's
 * nodes together - so no device-link comparison is needed.
 *
 * The numbers come from the entry's dev file rather than from stat()ing /dev/dri/card*, which cannot
 * work in the guest: its /dev/dri is a fabricated directory holding only the render node (a phone
 * app may not stat the real one - those nodes are graphics_device, which its policy does not grant),
 * so a card node is discoverable only through sysfs. */
static int icd_find_primary_for(const char *render_node, int64_t *pmaj, int64_t *pmin) {
    char device[128], dir[192];
    int64_t rmaj, rmin;
    if (!icd_node_device(render_node, &rmaj, &rmin, device, sizeof(device))) return 0;
    snprintf(dir, sizeof(dir), "/sys/dev/char/%lld:%lld/device/drm", (long long)rmaj, (long long)rmin);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !found) {
        char devfile[256];
        if (strncmp(e->d_name, "card", 4) != 0) continue;
        snprintf(devfile, sizeof(devfile), "%s/%s/dev", dir, e->d_name);
        FILE *f = fopen(devfile, "r");
        if (!f) continue;
        long cmaj = -1, cmin = -1;
        if (fscanf(f, "%ld:%ld", &cmaj, &cmin) == 2 && cmaj >= 0 && cmin >= 0) {
            *pmaj = cmaj; *pmin = cmin; found = 1;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* The DRM device behind the render node the HAL drives (PowerVR's is renderD128; nothing here
 * hardcodes that), and the primary node of that same device, if one is registered. */
static void icd_find_drm_dev(int *has_primary, int64_t *pmaj, int64_t *pmin, int64_t *rmaj, int64_t *rmin) {
    if (!g_drm_dev_known) {
        g_drm_dev_known = 1;
        for (int minor = 0; minor < 256; minor++) {
            char path[64];
            struct stat st;
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
            if (stat(path, &st) != 0) continue;
            g_drm_render_major = major(st.st_rdev);
            g_drm_render_minor = minor;
            if (icd_find_primary_for(path, &g_drm_primary_major, &g_drm_primary_minor)) g_drm_has_primary = 1;
            break;
        }
    }
    *has_primary = g_drm_has_primary;
    *pmaj = g_drm_primary_major; *pmin = g_drm_primary_minor;
    *rmaj = g_drm_render_major; *rmin = g_drm_render_minor;
}

/* The HAL refuses a DRM-format-modifier image query that also carries
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and the VkImageFormatListCreateInfo that goes with it, with
 * VK_ERROR_FEATURE_NOT_PRESENT:
 *
 *   SAMPLED|STORAGE|XFER_SRC                     -> VK_SUCCESS, EXPORTABLE|IMPORTABLE
 *   same + MUTABLE_FORMAT + format list           -> VK_ERROR_FEATURE_NOT_PRESENT
 *
 * gamescope asks the second way for its composited output: BInit sets that bit and list whenever an
 * image's UNORM and SRGB views differ, then treats any result other than VK_ERROR_FORMAT_NOT_SUPPORTED
 * as fatal, so it never reaches its own EXPORTABLE test and dies at "assert( modifiers.size() > 0 )".
 *
 * Mutable format describes a VIEW capability, not whether the format+modifier can be a dma-buf image
 * at all - which is what gamescope is really asking - so this retries that question without the two
 * bits, and only when the HAL's refusal is that specific error on a modifier-tiled query. */
#define VK_ERROR_FEATURE_NOT_PRESENT_ (-11)
#define VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT_ 1000158000u
#define VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_ 0x00000008u
#define STYPE_IMAGE_FORMAT_LIST 1000147000u
#define STYPE_EXT_IMG_FMT_INFO_ 1000071000u

typedef struct { uint32_t sType; void *pNext; uint32_t format, type, tiling, usage, flags; } ImgFmtInfo2;
typedef struct { uint32_t sType; void *pNext; } VkBase;
typedef struct { uint32_t sType; void *pNext; uint32_t viewFormatCount; const void *pViewFormats; } FmtList;

static void *icd_gipa_forward(void *instance, const char *name) {
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa ? g_gipa((VkInstance)instance, name) : NULL;
}

static VkResult icd_get_phys_dev_img_fmt_props2(void *pd, void *info, void *out) {
    VkResult (*hal)(void *, void *, void *) =
        (VkResult (*)(void *, void *, void *))icd_gipa_forward(g_instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = hal(pd, info, out);
    ImgFmtInfo2 *i = (ImgFmtInfo2 *)info;
    if (r != VK_ERROR_FEATURE_NOT_PRESENT_ || !i || i->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT_)
        return r;
    if (!(i->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_)) return r;

    /* Rebuild the pNext chain by UNLINKING the format-list struct from the live chain - each struct's
     * pNext is rewritten to skip it. (An array of pointers is not a pNext chain, and handing the HAL
     * one is what made an earlier attempt at this silently fail.) */
    ImgFmtInfo2 retry = *i;
    VkBase *prev = (VkBase *)&retry;
    for (VkBase *b = (VkBase *)i->pNext; b; b = (VkBase *)b->pNext) {
        if (b->sType == STYPE_IMAGE_FORMAT_LIST) { prev->pNext = b->pNext; continue; }
        prev->pNext = b; prev = b;
    }
    prev->pNext = NULL;
    retry.flags = i->flags & ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_;

    VkResult r2 = hal(pd, &retry, out);
    if (icd_debug_on())
        fprintf(stderr, "pvr_icd: DRM-modifier query fmt=0x%x usage=0x%x with MUTABLE_FORMAT -> %d; "
                        "retried without it -> %d\n", i->format, i->usage, r, r2);
    return (r2 == VK_SUCCESS) ? r2 : r;   /* still unsupported: keep the original answer */
}

static void icd_get_phys_dev_props2(void *pd, void *props2) {
    PhysDevProps2 *p2 = (PhysDevProps2 *)props2;
    if (!p2) return;
    /* The HAL answers for the rest of the chain; it just leaves this struct empty. */
    if (!g_gipa && icd_load() != 0) return;
    void (*hal)(void *, void *) = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    if (hal) hal(pd, props2);
    int has_primary; int64_t pmaj, pmin, rmaj, rmin;
    icd_find_drm_dev(&has_primary, &pmaj, &pmin, &rmaj, &rmin);
    for (PhysDevDrmProps *d = (PhysDevDrmProps *)p2->pNext; d; d = (PhysDevDrmProps *)d->pNext) {
        if (d->sType != STYPE_DRM_PROPERTIES_EXT) continue;
        d->hasPrimary = (uint32_t)has_primary;
        d->primaryMajor = pmaj; d->primaryMinor = pmin;
        d->hasRender = 1;
        d->renderMajor = rmaj; d->renderMinor = rmin;
        if (icd_debug_on())
            fprintf(stderr, "pvr_icd: physical_device_drm: hasPrimary=%d primary=%lld:%lld render=%lld:%lld\n",
                    has_primary, (long long)pmaj, (long long)pmin, (long long)rmaj, (long long)rmin);
    }
}

/* Intercept instance creation: make one for the HAL without our extension names in it. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags; const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} InstanceCreateInfo;
static VkResult icd_create_instance(const void *ci, const void *alloc, VkInstance *out) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal_create)(const void *, const void *, VkInstance *) =
        (VkResult (*)(const void *, const void *, VkInstance *))g_gipa(NULL, "vkCreateInstance");
    if (!hal_create) return VK_ERROR_INITIALIZATION_FAILED;
    const InstanceCreateInfo *in = (const InstanceCreateInfo *)ci;
    uint32_t n = in->enabledExtensionCount;
    const char *const *names = in->ppEnabledExtensionNames;
    VkInstance created = NULL;   /* only published once the HAL returned success */
    VkResult r;
    if (n == 0) {
        r = hal_create(ci, alloc, &created);
    } else {
        const char **kept = (const char **)malloc(sizeof(char *) * n);
        uint32_t kn = 0;
        for (uint32_t i = 0; i < n; i++) if (!icd_is_our_extension(names[i])) kept[kn++] = names[i];
        InstanceCreateInfo copy = *in;
        copy.enabledExtensionCount = kn;
        copy.ppEnabledExtensionNames = kept;
        r = hal_create(&copy, alloc, &created);
        free(kept);
    }
    if (r != VK_SUCCESS) return r;
    /* g_instance is the handle device procs are resolved against; a failed create must not leave a
     * stale one, and *out must not be handed a garbage handle. */
    g_instance = created;
    if (out) *out = created;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
int vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    /* We implement up to loader interface 5; the loader caps to what we report. */
    if (version && *version > 5) *version = 5;
    icd_load();
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
void *vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    void *wsi = pvr_wsi_proc(pName);
    if (wsi) return wsi;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (void *)icd_gdpa;
    if (!strcmp(pName, "vkGetPhysicalDeviceProcAddr")) return (void *)icd_gpdpa;
    if (!strcmp(pName, "vkCreateDevice")) return (void *)icd_create_device;
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void *)icd_enum_device_ext;
    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties")) return (void *)icd_enum_instance_ext;
    if (!strcmp(pName, "vkCreateInstance")) return (void *)icd_create_instance;
    /* The loader's own trampoline for vkGetPhysicalDeviceProperties2 calls whatever this returns, so
     * this is where the HAL's empty VK_EXT_physical_device_drm answer is corrected. */
    if (!strcmp(pName, "vkGetPhysicalDeviceProperties2") || !strcmp(pName, "vkGetPhysicalDeviceProperties2KHR"))
        return (void *)icd_get_phys_dev_props2;
    /* A DRM-modifier image query the HAL refuses only because of the mutable-format bits; see the
     * note above icd_get_phys_dev_img_fmt_props2. */
    if (!strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") ||
        !strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (void *)icd_get_phys_dev_img_fmt_props2;
    if (!g_gipa) { if (icd_load() != 0) return NULL; }
    return g_gipa(instance, pName);
}

__attribute__((visibility("default")))
void *vk_icdGetPhysicalDeviceProcAddr(VkPhysicalDevice pd, const char *pName) {
    (void)pd;
    if (!pName) return NULL;
    /* The loader asks here for physical-device functions — including the device-extension
     * enumeration it validates against. Ours must answer, so VK_KHR_swapchain is visible. */
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void *)icd_enum_device_ext;
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa(g_instance, pName);
}

/* A loader that implements ICD interface 1 (and every loader from 1.3.x on) calls vk_icdCreateInstance
 * DIRECTLY rather than fetching "vkCreateInstance" from vk_icdGetInstanceProcAddr. This used to hand
 * the app's create-info straight to the HAL, which meant VK_KHR_wayland_surface / VK_KHR_xcb_surface /
 * VK_KHR_xlib_surface reached the PowerVR blob -- it knows only Android surfaces, so the call failed and
 * SDL reported "VK_KHR_wayland_surface extension is not enabled in the Vulkan instance" and could not
 * create its backend, which is what stopped the Steam session. Route it through icd_create_instance,
 * which strips our extension names before the HAL sees them; the app still gets them on the instance
 * this ICD serves. */
__attribute__((visibility("default")))
int vk_icdCreateInstance(const void *ci, const void *alloc, VkInstance *out) {
    return (int)icd_create_instance(ci, alloc, out);
}
