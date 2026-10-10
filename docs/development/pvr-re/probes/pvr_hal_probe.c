/*
 * pvr_hal_probe.c — drive the PowerVR Vulkan HAL directly, bypassing the Android loader.
 *
 * /vendor/lib64/hw/vulkan.powervr.so is an Android hwvulkan HAL module: it exports the
 * symbol HMI (hw_module_t), whose methods->open() returns an hwvulkan_device_t carrying
 * GetInstanceProcAddr/CreateInstance. A glibc shim (libhybris-style) must open it the same
 * way, so this harness proves (a) the module opens and yields a working instance, and
 * (b) the DXVK-critical capability set (BCn/S3TC, ETC2/ASTC, dmabuf, modifiers).
 *
 * Built bionic in this step to isolate "does the module work at all" from the later
 * "does it work under glibc" question. Types are spelled out locally: no Vulkan loader
 * headers needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int32_t VkResult;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;

/* --- Android hardware module ABI (hardware/libhardware/include/hardware/hardware.h) --- */
typedef struct hw_module_t hw_module_t;
typedef struct hw_device_t { uint32_t tag; uint32_t version; hw_module_t *module; uint32_t reserved[12]; int (*close)(struct hw_device_t *); } hw_device_t;
typedef struct hw_module_methods_t { int (*open)(const hw_module_t *, const char *, hw_device_t **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id; const char *name; const char *author;
    hw_module_methods_t *methods; void *dso; uint32_t reserved[32 - 7];
};
#define HAL_MODULE_TAG 0x48570000u  /* MAKE_TAG2('H','W') */

/* hwvulkan_device_t: hw_device_t followed by entrypoints. The exact offsets are read from
 * vulkan_device_open() in the blob (0x1b9ee0): module at +0x08, close at +0x70,
 * EnumerateInstanceExtensionProperties at +0x78, CreateInstance at +0x80, GetInstanceProcAddr
 * at +0x88, in a 0x90-byte calloc. 18 pointer-sized words model it without guessing the header. */
typedef struct { uint64_t w[18]; } hwvulkan_device_t;
#define DEV_CREATE_INSTANCE(d) ((VkResult (*)(const void *, const void *, VkInstance *))((d)->w[0x80 / 8]))
#define DEV_GIPA(d) ((void *(*)(VkInstance, const char *))((d)->w[0x88 / 8]))

#define VK_SUCCESS 0
#define VK_MAKE_API_VERSION(v, va, ma, mi) ((((uint32_t)(v)) << 29) | (((uint32_t)(va)) << 22) | (((uint32_t)(ma)) << 12) | ((uint32_t)(mi)))
#define VK_API_VERSION_MAJOR(v) (((uint32_t)(v) >> 22) & 0x7FU)
#define VK_API_VERSION_MINOR(v) (((uint32_t)(v) >> 12) & 0x3FFU)

typedef struct { uint32_t sType; const void *pNext; const char *pApplicationName; uint32_t applicationVersion; const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const VkApplicationInfo *pApplicationInfo; uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames; uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames; } VkInstanceCreateInfo;
typedef struct { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;
typedef struct { uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType; char deviceName[256]; uint8_t uuid[16]; } VkPhysicalDevicePropertiesHead;
typedef struct { uint32_t linearTilingFeatures, optimalTilingFeatures, bufferFeatures; } VkFormatProperties;

/* A current Vulkan application on this device needs 1.1; 1.0 works everywhere. */
static void *dl(const char *name) { void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL); if (!h) fprintf(stderr, "  [dlopen %s: %s]\n", name, dlerror()); return h; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("== PowerVR HAL direct-drive probe ==\n");
    /* The module's own deps first, so they are resident under the names it expects. */
    const char *pre[] = { "libc++.so", "libcutils.so", "libion.so", "libsync.so", "libnativewindow.so",
                          "libdmabufheap.so", "libdrm.so", "liblog.so", "libsrv_um.so", "libusc.so",
                          "libufwriter.so", "libcustomer_gralloc_ddk_api.so" };
    for (unsigned i = 0; i < sizeof(pre) / sizeof(*pre); i++) if (dl(pre[i])) printf("  preload ok: %s\n", pre[i]);

    void *blob = dl("/vendor/lib64/hw/vulkan.powervr.so");
    if (!blob) { printf("FATAL: could not dlopen the blob: %s\n", dlerror()); return 2; }

    hw_module_t *mod = (hw_module_t *)dlsym(blob, "HMI");
    if (!mod) { printf("FATAL: no HMI symbol\n"); return 2; }
    printf("HMI tag=0x%08x api=%u.%u id=%s name=%s author=%s methods=%p\n",
           mod->tag, mod->module_api_version, mod->hal_api_version,
           mod->id ? mod->id : "(null)", mod->name ? mod->name : "(null)",
           mod->author ? mod->author : "(null)", (void *)mod->methods);
    if (!mod->methods || !mod->methods->open) { printf("FATAL: no open()\n"); return 2; }

    hwvulkan_device_t *dev = NULL;
    int rc = mod->methods->open(mod, "vk0", (hw_device_t **)&dev);
    printf("open(id=\"vk0\") rc=%d dev=%p\n", rc, (void *)dev);
    if (rc != 0 || !dev) { rc = mod->methods->open(mod, "vulkan", (hw_device_t **)&dev); printf("open(id=\"vulkan\") rc=%d dev=%p\n", rc, (void *)dev); }
    if (!dev) { printf("FATAL: HAL open returned no device\n"); return 2; }
    void *(*gipa)(VkInstance, const char *);
    VkResult (*createInstance)(const void *, const void *, VkInstance *);
    printf("hwvulkan device: tag=0x%08x version=0x%08x CreateInstance=%p GetInstanceProcAddr=%p\n",
           (uint32_t)dev->w[0], (uint32_t)(dev->w[0] >> 32), (void *)DEV_CREATE_INSTANCE(dev), (void *)DEV_GIPA(dev));

    gipa = DEV_GIPA(dev);
    createInstance = (void *)gipa(NULL, "vkCreateInstance");
    if (!createInstance) createInstance = DEV_CREATE_INSTANCE(dev);
    printf("procs(global): createInstance=%p\n", (void *)createInstance);

    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.sType = 0; ai.pApplicationName = "pvr-hal-probe"; ai.applicationVersion = 1; ai.pEngineName = "pvr-hal-probe"; ai.engineVersion = 1; ai.apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0);
    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof(ci)); ci.sType = 1; ci.pApplicationInfo = &ai;
    VkInstance inst = NULL;
    VkResult r = createInstance ? createInstance(&ci, NULL, &inst) : -1;
    printf("vkCreateInstance(1.1) -> %d inst=%p\n", r, (void *)inst);
    if (r != VK_SUCCESS || !inst) { printf("RESULT: HAL opens but cannot create an instance\n"); return 1; }

    /* Instance-level procs must be resolved with the instance, per the loader contract. */
    void (*getProps)(VkPhysicalDevice, void *) = (void *)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkResult (*enumPhys)(VkInstance, uint32_t *, VkPhysicalDevice *) = (void *)gipa(inst, "vkEnumeratePhysicalDevices");
    VkResult (*enumDevExt)(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *) = (void *)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    void (*getFeat)(VkPhysicalDevice, void *) = (void *)gipa(inst, "vkGetPhysicalDeviceFeatures");
    void (*getFmtProps)(VkPhysicalDevice, int, VkFormatProperties *) = (void *)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    printf("procs(instance): getProps=%p enumPhys=%p getFeat=%p getFmtProps=%p enumDevExt=%p\n",
           (void *)getProps, (void *)enumPhys, (void *)getFeat, (void *)getFmtProps, (void *)enumDevExt);
    if (!enumPhys || !getProps) { printf("RESULT: instance created but instance procs unresolved\n"); return 1; }

    uint32_t n = 0;
    enumPhys(inst, &n, NULL);
    VkPhysicalDevice pd = NULL; enumPhys(inst, &n, &pd);
    printf("physical devices: %u\n", n);
    /* VkPhysicalDeviceProperties: apiVersion/driverVersion/vendorID/deviceID/deviceType then name. */
    struct { uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType; char deviceName[256]; } p;
    memset(&p, 0, sizeof(p));
    getProps(pd, &p);
    printf("  device \"%s\" api=%u.%u.%u driver=%u(0x%x) vendor=0x%x device=0x%x type=%u\n",
           p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), p.apiVersion & 0xFFF,
           p.driverVersion, p.driverVersion, p.vendorID, p.deviceID, p.deviceType);

    /* DXVK-critical: BCn/S3TC texture compression. VkPhysicalDeviceFeatures.textureCompressionBC is bit 22. */
    struct { uint32_t f[64]; } feat; memset(&feat, 0, sizeof(feat));
    if (getFeat) getFeat(pd, &feat);
    printf("features: robustBufferAccess=%u textureCompressionETC2=%u textureCompressionASTC_LDR=%u textureCompressionBC=%u\n",
           feat.f[0] & 1, (feat.f[0] >> 20) & 1, (feat.f[0] >> 21) & 1, (feat.f[0] >> 22) & 1);

    struct { const char *name; int fmt; } fmts[] = {
        { "BC1_RGBA_UNORM", 131 }, { "BC2_UNORM", 135 }, { "BC3_UNORM", 137 }, { "BC7_UNORM", 145 },
        { "ETC2_R8G8B8A8_UNORM", 147 }, { "ASTC_4x4_UNORM", 157 }, { "R8G8B8A8_UNORM", 37 },
    };
    for (unsigned i = 0; i < sizeof(fmts) / sizeof(*fmts); i++) {
        VkFormatProperties fp; memset(&fp, 0, sizeof(fp));
        if (getFmtProps) getFmtProps(pd, fmts[i].fmt, &fp);
        printf("  fmt %-22s linear=0x%08x optimal=0x%08x buffer=0x%08x%s\n", fmts[i].name,
               fp.linearTilingFeatures, fp.optimalTilingFeatures, fp.bufferFeatures,
               fp.optimalTilingFeatures ? "  <-- SAMPLED" : "");
    }

    /* The extensions DroidDeck's compositor and DXVK need, straight from the HAL. */
    uint32_t en = 0; enumDevExt(pd, NULL, &en, NULL);
    VkExtensionProperties *eps = calloc(en ? en : 1, sizeof(*eps));
    enumDevExt(pd, NULL, &en, eps);
    const char *want[] = { "VK_EXT_image_drm_format_modifier", "VK_EXT_external_memory_dma_buf",
                           "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd", "VK_KHR_timeline_semaphore",
                           "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_physical_device_drm",
                           "VK_KHR_dynamic_rendering", "VK_EXT_descriptor_indexing" };
    printf("device extensions: %u\n", en);
    for (unsigned i = 0; i < sizeof(want) / sizeof(*want); i++) {
        int found = 0;
        for (uint32_t k = 0; k < en; k++) if (!strcmp(eps[k].extensionName, want[i])) { found = 1; break; }
        printf("  %-52s %s\n", want[i], found ? "present" : "MISSING");
    }
    free(eps);
    printf("RESULT: HAL opens, instance and device enumerate through the module's own entrypoints\n");
    return 0;
}
