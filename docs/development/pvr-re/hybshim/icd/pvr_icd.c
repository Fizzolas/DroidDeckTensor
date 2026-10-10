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

/* libhybris entry points (provided by libhybris.so, which this DSO links against). */
extern void *android_dlopen_ext(const char *filename, int flag, const void *info);
extern void *android_dlsym(void *handle, const char *symbol);
extern char *android_dlerror(void);

#define RTLD_NOW 2
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
static const char *const ICD_INSTANCE_EXTENSIONS[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
#define ICD_NUM_INSTANCE_EXTENSIONS 2

static void icd_log(const char *m) { fprintf(stderr, "pvr_icd: %s\n", m); }

/* The Wayland WSI this ICD carries (the HAL only does Android surfaces). */
extern void *pvr_wsi_proc(const char *name);
static int icd_load(void);   /* defined below; used by pvr_icd_resolve */

/* Resolve a dynamic Vulkan entrypoint through the HAL. The WSI (its own translation unit) uses this
 * instead of referencing the global vkGetDeviceProcAddr, which an ICD must not have undefined. */
static void *icd_gdpa(VkDevice device, const char *pName);   /* forward */
__attribute__((visibility("default")))
void *pvr_icd_resolve(void *instance, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)icd_gdpa;
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa((VkInstance)instance, name);
}

/* Load the bionic HAL once. Returns 0 on success. */
static int icd_load(void) {
    if (g_gipa) return 0;
    g_blob = android_dlopen_ext(BLOB, RTLD_NOW, NULL);
    if (!g_blob) { fprintf(stderr, "pvr_icd: android_dlopen_ext(%s) failed: %s\n", BLOB, android_dlerror()); return -1; }

    hw_module_t *mod = (hw_module_t *)android_dlsym(g_blob, "HMI");
    if (!mod || !mod->methods || !mod->methods->open) { icd_log("no HMI/open in the HAL"); return -1; }

    void *dev = NULL;
    int rc = mod->methods->open(mod, "vk0", &dev);
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

/* Intercept instance creation: make one for the HAL without our extension names in it. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags; const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} InstanceCreateInfo;
static VkResult icd_create_instance(const void *ci, const void *alloc, VkInstance *out) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    const InstanceCreateInfo *in = (const InstanceCreateInfo *)ci;
    VkResult (*hal_create)(const void *, const void *, VkInstance *) =
        (VkResult (*)(const void *, const void *, VkInstance *))g_gipa(NULL, "vkCreateInstance");
    uint32_t n = in->enabledExtensionCount;
    const char *const *names = in->ppEnabledExtensionNames;
    if (n == 0) {
        VkResult r = hal_create(ci, alloc, &g_instance);
        if (out) *out = g_instance;
        return r;
    }
    const char **kept = (const char **)malloc(sizeof(char *) * n);
    uint32_t kn = 0;
    for (uint32_t i = 0; i < n; i++) if (!icd_is_our_extension(names[i])) kept[kn++] = names[i];
    InstanceCreateInfo copy = *in;
    copy.enabledExtensionCount = kn;
    copy.ppEnabledExtensionNames = kept;
    VkResult r = hal_create(&copy, alloc, &g_instance);
    free(kept);
    if (out) *out = g_instance;
    return r;
}

__attribute__((visibility("default")))
int vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    /* We implement up to loader interface 5; the loader caps to what we report. */
    if (version && *version > 3) *version = 3;
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

/* Some loaders call these directly rather than via GetInstanceProcAddr. */
__attribute__((visibility("default")))
int vk_icdCreateInstance(const void *ci, const void *alloc, VkInstance *out) {
    if (icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    void *fn = g_gipa(NULL, "vkCreateInstance");
    if (!fn) fn = (void *)g_create;
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    return ((int (*)(const void *, const void *, VkInstance *))fn)(ci, alloc, out);
}
