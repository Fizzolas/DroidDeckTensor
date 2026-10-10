/* pvr_drm_props_probe.c — what VK_EXT_physical_device_drm the PowerVR HAL actually reports.
 *
 * gamescope's Wayland backend refuses to come up unless the physical device has a DRM PRIMARY node
 * (rendervulkan.cpp: "physical device has no primary node"), and falls back to its SDL backend -
 * which is what then dies with "VK_KHR_wayland_surface extension is not enabled in the Vulkan
 * instance". A nested compositor like gamescope-on-Wayland has no business needing a primary node
 * (that is a KMS scanout path), so the right fix depends on whether the HAL reports hasPrimary=false
 * honestly, or reports no DRM properties at all and the ICD must synthesise them.
 *
 * Run it through the ICD (so it sees what gamescope sees, not the HAL directly):
 *   ./lib/ld-linux-aarch64.so.1 --library-path ./lib:./linkerdir ./bin/pvr_drm_props_probe
 * Env: VK_ICD_FILENAMES=<our manifest>, HYBRIS_LINKER_DIR, HYBRIS_LD_LIBRARY_PATH.
 *
 * Build (from WSL; zig needs the real Vulkan headers, see build_probe.sh):
 *   zig cc -target aarch64-linux-gnu.2.38 -O1 -o pvr_drm_props_probe pvr_drm_props_probe.c \
 *     -I<mesa>/include -ldl
 *
 * Use the real headers, never a hand-rolled struct. A driver writes the WHOLE
 * VkPhysicalDeviceProperties it is handed, so a shortened stand-in is written past its end and the
 * pNext chain then reads stack that the call just clobbered - which is exactly how this probe once
 * reported hasPrimary=2048 (that is VkPhysicalDeviceProperties' own maxImageDimension1D).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#define VKMK(v,va,ma,mi) ((((uint32_t)(v))<<29)|(((uint32_t)(va))<<22)|(((uint32_t)(ma))<<12)|((uint32_t)(mi)))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== VK_EXT_physical_device_drm as the ICD reports it ==\n");

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("dlopen(libvulkan.so.1) failed: %s\n", dlerror()); _exit(2); }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr\n"); _exit(2); }

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "drm-props-probe", .applicationVersion = 1,
        .pEngineName = "none", .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = gipa(NULL, "vkCreateInstance") ? ((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst)
                                                 : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS || !inst) { printf("vkCreateInstance -> %d\n", r); _exit(1); }

    PFN_vkEnumeratePhysicalDevices enumPhys = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (!enumPhys || !enumDevExt) { printf("instance procs unresolved\n"); _exit(1); }

    uint32_t c = 0;
    if (enumPhys(inst, &c, NULL) != VK_SUCCESS || !c) { printf("no physical device\n"); _exit(1); }
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    enumPhys(inst, &c, &pd);
    if (!pd) { printf("no physical device\n"); _exit(1); }

    /* Is the extension even advertised? gamescope only queries the struct if it is. */
    uint32_t n = 0;
    enumDevExt(pd, NULL, &n, NULL);
    VkExtensionProperties *all = n ? calloc(n, sizeof(*all)) : NULL;
    int advertised = 0;
    if (all) {
        enumDevExt(pd, NULL, &n, all);
        for (uint32_t i = 0; i < n; i++) if (!strcmp(all[i].extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) advertised = 1;
        free(all);
    }
    printf("advertised %s: %s (%u device extensions)\n", VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
           advertised ? "yes" : "NO", n);

    if (!advertised) {
        printf("RESULT: not advertised - gamescope never even asks, and calls the device invalid\n");
        _exit(0);
    }

    /* Poison the whole chain first: a driver that ignores the struct leaves the poison behind, and
     * the raw words then say so instead of looking like a plausible answer. */
    VkPhysicalDeviceDrmPropertiesEXT d;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0xAB, sizeof(p2));
    memset(&d, 0xCD, sizeof(d));
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &d;
    d.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
    d.pNext = NULL;

    PFN_vkGetPhysicalDeviceProperties2 getProps2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!getProps2) getProps2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2KHR");
    if (!getProps2) { printf("no vkGetPhysicalDeviceProperties2\n"); _exit(1); }
    getProps2(pd, &p2);

    printf("device: \"%.*s\" api=%u.%u.%u\n", 64, p2.properties.deviceName,
           VK_VERSION_MAJOR(p2.properties.apiVersion), VK_VERSION_MINOR(p2.properties.apiVersion),
           VK_VERSION_PATCH(p2.properties.apiVersion));
    printf("hasPrimary=%u hasRender=%u\n", d.hasPrimary, d.hasRender);
    printf("primary=%lld:%lld  render=%lld:%lld\n",
           (long long)d.primaryMajor, (long long)d.primaryMinor,
           (long long)d.renderMajor, (long long)d.renderMinor);
    /* 0xCDCDCDCD in a field means nothing wrote it: the driver never filled this struct at all, and
     * any "hasPrimary" read out of it is our own poison. */
    const uint32_t *raw = (const uint32_t *)&d;
    printf("raw drmProps words:");
    for (unsigned i = 0; i < sizeof(d) / 4; i++) printf(" %08x", raw[i]);
    printf("\n");

    if (d.sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT)
        printf("RESULT: the driver OVERWROTE sType (%08x != %08x) - it does not understand this struct\n",
               d.sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT);
    else if (d.hasPrimary == 0xCDCDCDCDu)
        printf("RESULT: the driver left the struct untouched (poison intact) - it never fills it\n");
    else if (d.hasPrimary)
        printf("RESULT: primary node IS reported (primary=%lld:%lld)\n",
               (long long)d.primaryMajor, (long long)d.primaryMinor);
    else
        printf("RESULT: hasPrimary FALSE, honestly reported - a nested backend must not require it\n");
    printf("(run `ls -l /dev/dri` in this guest to compare renderMajor:renderMinor with what exists)\n");
    _exit(0);
}
