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

typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;

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

static void icd_log(const char *m) { fprintf(stderr, "pvr_icd: %s\n", m); }

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
    if (!g_gipa) { if (icd_load() != 0) return NULL; }
    /* The loader resolves global + instance procs through here; the HAL knows both forms. */
    return g_gipa(instance, pName);
}

__attribute__((visibility("default")))
void *vk_icdGetPhysicalDeviceProcAddr(VkPhysicalDevice pd, const char *pName) {
    (void)pd; (void)pName;
    /* The HAL exposes device-level procs through the same GetInstanceProcAddr. */
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa(NULL, pName);
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
