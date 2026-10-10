/* pvr_fmt_props_probe.c — what the PowerVR HAL reports for DRM format modifiers and exportability.
 *
 * gamescope allocates its composited output as a flippable dmabuf texture. For each candidate
 * modifier it keeps only those whose VkExternalImageFormatProperties reports
 * VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT, then asserts the list is non-empty:
 *
 *   src/rendervulkan.cpp:2264  assert( modifiers.size() > 0 );
 *
 * and, when the modifier list itself is empty, logs first:
 *
 *   "vkGetPhysicalDeviceFormatProperties2 returned zero modifiers for DRM format 0x..."
 *
 * Both come from two calls the HAL answers:
 *   1. vkGetPhysicalDeviceFormatProperties2 with VkDrmFormatModifierPropertiesListEXT in pNext
 *   2. vkGetPhysicalDeviceImageFormatProperties2 with VkPhysicalDeviceExternalImageFormatInfo
 *      (handleType = DMA_BUF) and VkPhysicalDeviceImageDrmFormatModifierInfoEXT in its pNext
 * The HAL has form here - it advertises VK_EXT_physical_device_drm and leaves that struct empty -
 * so this measures exactly what it fills in for each.
 *
 * Run it through the ICD:
 *   ./lib/ld-linux-aarch64.so.1 --library-path ./lib:./linkerdir ./bin/pvr_fmt_props_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

static const char *fmtname(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM (AR24)";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM (AB24)";
    case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_PACK32";
    case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return "G8_B8R8_2PLANE_420 (NV12)";
    default: return "?";
    }
}

/* Call 2: is [modifier] importable/exportable as a DMA_BUF image? gamescope's getModifierProps().
 *
 * The struct is ZEROED, not poisoned: a driver that never writes it then leaves 0, which is exactly
 * what gamescope reads - so a 0 here means "gamescope sees no EXPORTABLE bit", whether the HAL
 * wrote a zero or wrote nothing. Zero is the honest reproduction of the caller's view. */
static void probe_modifier(VkPhysicalDevice pd, PFN_vkGetPhysicalDeviceImageFormatProperties2 get2,
                           VkFormat fmt, uint64_t modifier, VkImageUsageFlags usage, const char *label) {
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkPhysicalDeviceExternalImageFormatInfo extInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modInfo, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &extInfo,
        .format = fmt, .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage };

    VkExternalImageFormatProperties ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
    VkImageFormatProperties2 out = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext };

    VkResult r = get2(pd, &info, &out);
    uint32_t f = ext.externalMemoryProperties.externalMemoryFeatures;
    printf("       %-34s r=%d features=0x%08x EXPORTABLE=%d IMPORTABLE=%d\n",
           label, r, f, (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0,
           (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0);
}

static void probe(VkPhysicalDevice pd, PFN_vkGetPhysicalDeviceFormatProperties2 get2,
                  PFN_vkGetPhysicalDeviceImageFormatProperties2 get3, VkFormat fmt) {
    uint32_t count = 0;
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT };
    VkFormatProperties2 fp = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list };
    get2(pd, fmt, &fp);
    printf("\n=== %s ===\n  modifiers reported: %u\n", fmtname(fmt), list.drmFormatModifierCount);
    if (list.drmFormatModifierCount == 0) return;

    VkDrmFormatModifierPropertiesEXT *props = calloc(list.drmFormatModifierCount, sizeof(*props));
    list.pDrmFormatModifierProperties = props;
    get2(pd, fmt, &fp);

    for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        printf("  [%2u] modifier=0x%016llx planes=%u tilingFeatures=0x%08x SAMPLED=%d COLOR_ATT=%d\n",
               i, (unsigned long long)props[i].drmFormatModifier,
               props[i].drmFormatModifierPlaneCount, props[i].drmFormatModifierTilingFeatures,
               (props[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0,
               (props[i].drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0);
        probe_modifier(pd, get3, fmt, props[i].drmFormatModifier,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "SAMPLED|COLOR_ATT (BInit)");
        probe_modifier(pd, get3, fmt, props[i].drmFormatModifier,
                       VK_IMAGE_USAGE_SAMPLED_BIT, "SAMPLED only");
        probe_modifier(pd, get3, fmt, props[i].drmFormatModifier,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACH only");
        probe_modifier(pd, get3, fmt, props[i].drmFormatModifier, 0, "usage = 0");
        /* Exactly the output texture BInit builds: bFlippable -> bExportable, and bStorage,
         * bTransferSrc, bSampled all set (vulkan_make_output). */
        probe_modifier(pd, get3, fmt, props[i].drmFormatModifier,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "SAMPLED|STORAGE|XFER_SRC (output)");
        /* And with the VkImageFormatListCreateInfo + MUTABLE_FORMAT that gamescope's
         * getModifierProps() forwards from BInit - the one difference from the call above. */
        {
            VkFormat views[2] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB };
            VkImageFormatListCreateInfo fl = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                .viewFormatCount = 2, .pViewFormats = views };
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
                .drmFormatModifier = props[i].drmFormatModifier, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            VkPhysicalDeviceExternalImageFormatInfo ei = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
                .pNext = &mi, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
            fl.pNext = &ei;
            VkPhysicalDeviceImageFormatInfo2 inf = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &fl,
                .format = fmt, .type = VK_IMAGE_TYPE_2D,
                .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT };
            VkExternalImageFormatProperties ext = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
            VkImageFormatProperties2 out = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &ext };
            VkResult r = get3(pd, &inf, &out);
            printf("       %-34s r=%d features=0x%08x EXPORTABLE=%d\n",
                   "  +MUTABLE_FORMAT+formatList", r, ext.externalMemoryProperties.externalMemoryFeatures,
                   (ext.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0);
        }
    }
    free(props);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== DRM format modifiers / exportability as the ICD reports them ==\n");

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("dlopen: %s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no GIPA\n"); return 2; }

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "fmt-props", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n"); return 1; }

    uint32_t n = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd);

    /* Is the modifier extension even advertised? gamescope only takes this path if it is. */
    uint32_t ec = 0;
    PFN_vkEnumerateDeviceExtensionProperties eep =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    eep(pd, NULL, &ec, NULL);
    VkExtensionProperties *ea = ec ? calloc(ec, sizeof(*ea)) : NULL;
    int mod = 0, extmem = 0;
    if (ea) {
        eep(pd, NULL, &ec, ea);
        for (uint32_t i = 0; i < ec; i++) {
            if (!strcmp(ea[i].extensionName, "VK_EXT_image_drm_format_modifier")) mod = 1;
            if (!strcmp(ea[i].extensionName, "VK_EXT_external_memory_dma_buf")) extmem = 1;
        }
        free(ea);
    }
    printf("VK_EXT_image_drm_format_modifier: %s   VK_EXT_external_memory_dma_buf: %s\n",
           mod ? "advertised" : "NO", extmem ? "advertised" : "NO");

    PFN_vkGetPhysicalDeviceFormatProperties2 get2 =
        (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(inst, "vkGetPhysicalDeviceFormatProperties2");
    PFN_vkGetPhysicalDeviceImageFormatProperties2 get3 =
        (PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(inst, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!get2 || !get3) { printf("missing format-properties entrypoints\n"); return 1; }

    probe(pd, get2, get3, VK_FORMAT_B8G8R8A8_UNORM);
    probe(pd, get2, get3, VK_FORMAT_R8G8B8A8_UNORM);
    probe(pd, get2, get3, VK_FORMAT_B8G8R8A8_SRGB);
    probe(pd, get2, get3, VK_FORMAT_R8G8B8A8_SRGB);
    probe(pd, get2, get3, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM);

    /* Which single usage bit (if any) makes the DRM-modifier DMA_BUF query fail? One modifier that
     * is known to pass is used, so the usage is the only variable. */
    {
        static const struct { VkImageUsageFlags u; const char *n; } bits[] = {
            { VK_IMAGE_USAGE_TRANSFER_SRC_BIT,     "TRANSFER_SRC" },
            { VK_IMAGE_USAGE_TRANSFER_DST_BIT,     "TRANSFER_DST" },
            { VK_IMAGE_USAGE_SAMPLED_BIT,          "SAMPLED" },
            { VK_IMAGE_USAGE_STORAGE_BIT,          "STORAGE" },
            { VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT" },
            { VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, "DEPTH_STENCIL" },
            { VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, "INPUT_ATTACHMENT" },
        };
        printf("\n=== usage sweep, B8G8R8A8_UNORM + LINEAR + DMA_BUF + DRM modifier tiling ===\n");
        for (unsigned k = 0; k < sizeof(bits) / sizeof(*bits); k++)
            probe_modifier(pd, get3, VK_FORMAT_B8G8R8A8_UNORM, 0 /* LINEAR */, bits[k].u, bits[k].n);
        probe_modifier(pd, get3, VK_FORMAT_B8G8R8A8_UNORM, 0, VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT, "SAMPLED|TRANSFER_SRC");
        probe_modifier(pd, get3, VK_FORMAT_B8G8R8A8_UNORM, 0, VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       "SAMPLED|TRANSFER_SRC|DST (0xd)");
    }
    printf("\n=== isolate: MUTABLE_FORMAT bit vs format-list struct (AR24 + LINEAR) ===\n");
    return 0;
}
