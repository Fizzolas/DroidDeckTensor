/* pvr_wsi.c — Wayland WSI for the PowerVR shim ICD.
 *
 * The vendor HAL implements only VK_KHR_android_surface, so the guest's Wayland WSI lives here. It
 * uses libwayland-client, and presents each frame as an exportable dma-buf bound into a wl_buffer via
 * zwp_linux_dmabuf_v1 -- the same path the dma-buf tests proved works. Only the Wayland path is done;
 * X11 (gamescope Xwayland) is a separate, later piece.
 *
 * The entrypoints follow the loader's ICD contract: they are returned from
 * vk_icdGetInstanceProcAddr for instance-level names, and stored on the instance for device level.
 */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <wayland-client.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WSI_LOG(...) do { fprintf(stderr, "pvr_wsi: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* Resolve a device entrypoint through the HAL (never the global vkGetDeviceProcAddr). */
extern void *pvr_icd_resolve(void *instance, const char *name);
static void *dev_proc(VkDevice dev, const char *name) {
    static PFN_vkGetDeviceProcAddr gdpa;
    if (!gdpa) gdpa = (PFN_vkGetDeviceProcAddr)pvr_icd_resolve(NULL, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(dev, name) : NULL;
}

/* ---- per-surface ---------------------------------------------------------------- */

struct wsi_surface {
    VkSurfaceKHR handle;          /* set to (VkSurfaceKHR)this by the ICD */
    struct wl_display *display;
    struct wl_surface *surface;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    int32_t width, height;
};

/* DRM fourcc the wl_buffer is described with, from the swapchain's VkFormat. */
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_FORMAT_ABGR8888 0x34324241u
static uint32_t drm_format_for(VkFormat f) {
    return (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB) ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_ARGB8888;
}

/* A minimal global registry so a surface can find zwp_linux_dmabuf_v1 on the app's display. */
struct wsi_display {
    struct wl_display *display;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct wl_registry *registry;
};
static struct wl_registry_listener g_reg_listener;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver);
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name);
static struct wl_registry_listener g_reg_listener = { reg_global, reg_remove };

/* Track one display->dmabuf binding, discovery done once per display. */
static struct wsi_display g_displays[8];
static int g_ndisplays;

static struct wsi_display *wsi_for_display(struct wl_display *d) {
    for (int i = 0; i < g_ndisplays; i++) if (g_displays[i].display == d) return &g_displays[i];
    if (g_ndisplays >= 8) return NULL;
    struct wsi_display *wd = &g_displays[g_ndisplays++];
    wd->display = d;
    wd->registry = wl_display_get_registry(d);
    wl_registry_add_listener(wd->registry, &g_reg_listener, wd);
    wl_display_roundtrip(d);   /* fire the globals */
    wl_display_roundtrip(d);   /* dmabuf format events */
    return wd;
}

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    struct wsi_display *wd = data;
    if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        wd->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }

/* ---- vkCreateWaylandSurfaceKHR -------------------------------------------------- */

static VkResult wsi_create_wayland_surface(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *ci,
                                           const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)instance; (void)alloc;
    if (!ci || !ci->display || !ci->surface) return VK_ERROR_INITIALIZATION_FAILED;
    struct wsi_display *wd = wsi_for_display(ci->display);
    struct wsi_surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->display = ci->display;
    s->surface = ci->surface;
    s->dmabuf = wd ? wd->dmabuf : NULL;
    s->width = 1280; s->height = 720;
    if (!s->dmabuf) WSI_LOG("no zwp_linux_dmabuf_v1 on the display; present will be unavailable");
    *out = (VkSurfaceKHR)s;
    return VK_SUCCESS;
}

static void wsi_destroy_surface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *alloc) {
    (void)instance; (void)alloc;
    free((struct wsi_surface *)surface);
}

/* ---- surface queries ------------------------------------------------------------ */

static VkResult wsi_surface_caps(VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
    (void)pd;
    struct wsi_surface *s = (struct wsi_surface *)surface;
    memset(caps, 0, sizeof(*caps));
    caps->minImageCount = 2;
    caps->maxImageCount = 3;
    caps->currentExtent.width = (uint32_t)s->width;
    caps->currentExtent.height = (uint32_t)s->height;
    caps->minImageExtent = (VkExtent2D){1, 1};
    caps->maxImageExtent = (VkExtent2D){4096, 4096};
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return VK_SUCCESS;
}

static VkResult wsi_surface_formats(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkSurfaceFormatKHR *formats) {
    (void)pd; (void)surface;
    static const VkSurfaceFormatKHR f[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    if (!formats) { *count = 2; return VK_SUCCESS; }
    uint32_t n = *count < 2 ? *count : 2;
    memcpy(formats, f, n * sizeof(*f));
    *count = n;
    return (*count < 2) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult wsi_surface_present_modes(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *count, VkPresentModeKHR *modes) {
    (void)pd; (void)surface;
    if (!modes) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) { *count = 0; return VK_INCOMPLETE; }
    modes[0] = VK_PRESENT_MODE_FIFO_KHR;
    *count = 1;
    return VK_SUCCESS;
}

/* ---- swapchain ------------------------------------------------------------------ */

struct wsi_swapchain_image { VkImage image; VkDeviceMemory mem; int fd; struct wl_buffer *buffer; };
struct wsi_swapchain {
    VkSwapchainKHR handle;
    VkDevice device;
    struct wsi_surface *surface;
    uint32_t count, next;
    int32_t width, height;
    uint32_t drm_format;
    struct wsi_swapchain_image images[3];
    PFN_vkCreateImage CreateImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR; PFN_vkGetPhysicalDeviceMemoryProperties GetPhysDevMemProps;
    VkPhysicalDeviceMemoryProperties memProps;
};

/* One fd per buffer, carried to the release listener through wl_buffer user_data. */
struct wsi_buffer_binding { struct wsi_swapchain *sc; uint32_t index; };

static void buffer_release(void *data, struct wl_buffer *buffer) {
    struct wsi_buffer_binding *b = data;
    if (b && b->sc) b->sc->images[b->index].buffer = NULL;  /* reusable again */
    wl_buffer_destroy(buffer);
    free(b);
}
static const struct wl_buffer_listener g_buffer_listener = { buffer_release };

static VkResult wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                     const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    struct wsi_surface *surf = (struct wsi_surface *)ci->surface;
    struct wsi_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->device = device; sc->surface = surf; sc->next = 0;
    sc->width = ci->imageExtent.width; sc->height = ci->imageExtent.height;
    sc->drm_format = drm_format_for(ci->imageFormat);
    sc->count = ci->minImageCount > 3 ? 3 : (ci->minImageCount < 2 ? 2 : ci->minImageCount);
    sc->CreateImage = (PFN_vkCreateImage)dev_proc(device, "vkCreateImage");
    sc->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)dev_proc(device, "vkGetImageMemoryRequirements");
    sc->AllocateMemory = (PFN_vkAllocateMemory)dev_proc(device, "vkAllocateMemory");
    sc->BindImageMemory = (PFN_vkBindImageMemory)dev_proc(device, "vkBindImageMemory");
    sc->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)dev_proc(device, "vkGetMemoryFdKHR");
    (void)sc->GetPhysDevMemProps;

    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = ci->imageFormat;
    ici.extent = (VkExtent3D){ (uint32_t)sc->width, (uint32_t)sc->height, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR; ici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    for (uint32_t i = 0; i < sc->count; i++) {
        VkResult r = sc->CreateImage(device, &ici, alloc, &sc->images[i].image);
        if (r != VK_SUCCESS) { WSI_LOG("vkCreateImage failed %d", r); return r; }
        VkMemoryRequirements mr; sc->GetImageMemoryRequirements(device, sc->images[i].image, &mr);
        VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        ded.pNext = &expo; ded.image = sc->images[i].image;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.pNext = &ded; mai.allocationSize = mr.size; mai.memoryTypeIndex = 0;
        for (uint32_t t = 0; t < 32; t++) if (mr.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
        r = sc->AllocateMemory(device, &mai, alloc, &sc->images[i].mem);
        if (r != VK_SUCCESS) { WSI_LOG("vkAllocateMemory failed %d", r); return r; }
        r = sc->BindImageMemory(device, sc->images[i].image, sc->images[i].mem, 0);
        if (r != VK_SUCCESS) return r;
        VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        gfi.memory = sc->images[i].mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (sc->GetMemoryFdKHR) sc->GetMemoryFdKHR(device, &gfi, &sc->images[i].fd); else sc->images[i].fd = -1;
    }
    WSI_LOG("swapchain: %u images %dx%d fmt %d (fd0=%d)", sc->count, sc->width, sc->height, ci->imageFormat, sc->images[0].fd);
    *out = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
}

static void wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR sw, const VkAllocationCallbacks *alloc) {
    (void)device; (void)alloc;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    if (sc) free(sc);
}

static VkResult wsi_get_swapchain_images(VkDevice device, VkSwapchainKHR sw, uint32_t *count, VkImage *images) {
    (void)device;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    if (!images) { *count = sc->count; return VK_SUCCESS; }
    uint32_t n = *count < sc->count ? *count : sc->count;
    for (uint32_t i = 0; i < n; i++) images[i] = sc->images[i].image;
    *count = n;
    return VK_SUCCESS;
}

static VkResult wsi_acquire_next_image(VkDevice device, VkSwapchainKHR sw, uint64_t timeout,
                                       VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)device; (void)timeout; (void)sem; (void)fence;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    *index = sc->next;
    sc->next = (sc->next + 1) % sc->count;
    return VK_SUCCESS;
}

/* ---- present: dmabuf fd -> wl_buffer -> attach -> commit ------------------------ */

static VkResult wsi_queue_present(const VkPresentInfoKHR *pi) {
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct wsi_swapchain *sc = (struct wsi_swapchain *)pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        struct wsi_surface *surf = sc->surface;
        if (!surf->dmabuf || sc->images[idx].fd < 0) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(surf->dmabuf);
        zwp_linux_buffer_params_v1_add(params, sc->images[idx].fd, 0, 0, sc->width * 4, 0, 0);
        struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(params, sc->width, sc->height, sc->drm_format, 0 /* LINEAR modifier */);
        zwp_linux_buffer_params_v1_destroy(params);
        if (!buf) { if (pi->pResults) pi->pResults[i] = VK_ERROR_OUT_OF_HOST_MEMORY; continue; }
        struct wsi_buffer_binding *b = calloc(1, sizeof(*b)); b->sc = sc; b->index = idx;
        sc->images[idx].buffer = buf;
        wl_buffer_add_listener(buf, &g_buffer_listener, b);
        wl_surface_attach(surf->surface, buf, 0, 0);
        wl_surface_damage_buffer(surf->surface, 0, 0, sc->width, sc->height);
        wl_surface_commit(surf->surface);
        wl_display_flush(surf->display);
    }
    if (pi->pResults) for (uint32_t i = 0; i < pi->swapchainCount; i++) if (pi->pResults[i] == VK_SUCCESS) pi->pResults[i] = VK_SUCCESS;
    return VK_SUCCESS;
}

/* ---- query the loader asks for, that the HAL cannot answer ---------------------- */

static VkResult wsi_present_support(VkPhysicalDevice pd, uint32_t qf, VkSurfaceKHR surface, VkBool32 *supported) {
    (void)pd; (void)qf; (void)surface;
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

/* ---- dispatch table ------------------------------------------------------------- */

void *pvr_wsi_proc(const char *name) {
    struct ent { const char *n; void *p; } t[] = {
        { "vkCreateWaylandSurfaceKHR", (void *)wsi_create_wayland_surface },
        { "vkDestroySurfaceKHR", (void *)wsi_destroy_surface },
        { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (void *)wsi_surface_caps },
        { "vkGetPhysicalDeviceSurfaceFormatsKHR", (void *)wsi_surface_formats },
        { "vkGetPhysicalDeviceSurfacePresentModesKHR", (void *)wsi_surface_present_modes },
        { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (void *)wsi_present_support },
        { "vkGetPhysicalDeviceSurfaceSupportKHR", (void *)wsi_present_support },
        { "vkCreateSwapchainKHR", (void *)wsi_create_swapchain },
        { "vkDestroySwapchainKHR", (void *)wsi_destroy_swapchain },
        { "vkGetSwapchainImagesKHR", (void *)wsi_get_swapchain_images },
        { "vkAcquireNextImageKHR", (void *)wsi_acquire_next_image },
        { "vkQueuePresentKHR", (void *)wsi_queue_present },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(*t); i++) if (!strcmp(name, t[i].n)) return t[i].p;
    return NULL;
}
