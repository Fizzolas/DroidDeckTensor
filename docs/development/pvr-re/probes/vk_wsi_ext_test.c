/* vk_wsi_ext_test.c — create a Vulkan instance WITH the window-system extensions enabled.
 *
 * Every other probe in here passes VK_NULL_HANDLE / an empty extension list, so the ICD's
 * instance-extension seam was never exercised. A real client (SDL, gamescope, the Steam client)
 * must enable VK_KHR_surface plus the platform one for its window system -- and a loader that
 * implements ICD interface 1 reaches vk_icdCreateInstance directly. When that entrypoint forwarded
 * the app's list untouched it handed VK_KHR_wayland_surface to the PowerVR HAL, which knows only
 * android surfaces; instance creation then failed and SDL reported
 *   "VK_KHR_wayland_surface extension is not enabled in the Vulkan instance"
 * which is what stopped the Steam session.
 *
 * This asserts the two things that matter: the extension is advertised, and an instance created
 * with it enabled succeeds.
 *
 * Env: VK_ICD_FILENAMES=<manifest>, HYBRIS_LINKER_DIR, HYBRIS_LD_LIBRARY_PATH.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>

typedef int32_t VkResult;
typedef struct VkInstance_T *VkInstance;
typedef struct { uint32_t sType; const void *pNext; const char *pApp; uint32_t appVer; const char *pEngine; uint32_t engVer; uint32_t api; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const VkApplicationInfo *pApp; uint32_t nLayers; const char *const *pLayers; uint32_t nExt; const char *const *pExt; } VkInstanceCreateInfo;
typedef struct { char name[256]; uint32_t specVersion; } VkExtensionProperties;
#define VKMK(v,va,ma,mi) ((((uint32_t)(v))<<29)|(((uint32_t)(va))<<22)|(((uint32_t)(ma))<<12)|((uint32_t)(mi)))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== instance create with WSI extensions enabled ==\n");

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("dlopen(libvulkan.so.1) failed: %s\n", dlerror()); _exit(2); }
    void *(*gipa)(VkInstance, const char *) = dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr in the loader\n"); _exit(2); }

    /* Advertised? The loader merges the ICD's instance extensions, so all four must be visible. */
    VkResult (*enumExt)(const char *, uint32_t *, VkExtensionProperties *) =
        (void *)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    if (!enumExt || enumExt(NULL, &n, NULL) != 0) { printf("enum instance extensions failed\n"); _exit(2); }
    VkExtensionProperties *all = calloc(n, sizeof(*all));
    enumExt(NULL, &n, all);
    const char *want[] = { "VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface" };
    int missing = 0;
    for (unsigned i = 0; i < sizeof(want) / sizeof(*want); i++) {
        int found = 0;
        for (uint32_t k = 0; k < n; k++) if (!strcmp(all[k].name, want[i])) { found = 1; break; }
        printf("  advertised %-24s %s\n", want[i], found ? "yes" : "NO");
        if (!found) missing = 1;
    }
    if (missing) { printf("RESULT: FAIL - the ICD does not advertise its WSI extensions\n"); _exit(1); }

    /* Enabled: the create must succeed. This is the path that killed the Steam session. */
    const char *exts[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.pApp = "vk-wsi-ext-test"; ai.appVer = 1; ai.pEngine = "none"; ai.engVer = 1; ai.api = VKMK(0, 1, 1, 0);
    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof(ci));
    ci.sType = 1; ci.pApp = &ai; ci.nExt = 2; ci.pExt = exts;
    VkInstance inst = NULL;
    VkResult r = ((VkResult (*)(const void *, const void *, VkInstance *))gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst);
    printf("vkCreateInstance(VK_KHR_surface + VK_KHR_wayland_surface) -> %d inst=%p\n", r, (void *)inst);
    if (r != 0 || !inst) {
        printf("RESULT: FAIL - a client could not enable the WSI extensions it needs\n");
        _exit(1);
    }
    /* And the surface entrypoint must resolve on that instance. */
    void *createSurface = gipa(inst, "vkCreateWaylandSurfaceKHR");
    printf("vkCreateWaylandSurfaceKHR resolved: %s\n", createSurface ? "yes" : "NO");
    if (!createSurface) { printf("RESULT: FAIL - no wayland surface entrypoint\n"); _exit(1); }

    printf("RESULT: SUCCESS - WSI extensions advertise, enable, and resolve\n");
    _exit(0);
}
