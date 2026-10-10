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
#define VK_USE_PLATFORM_XCB_KHR 1
#define VK_USE_PLATFORM_XLIB_KHR 1
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include <wayland-client.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define WSI_LOG(...) do { fprintf(stderr, "pvr_wsi: " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* Resolve a device entrypoint through the HAL (never the global vkGetDeviceProcAddr). */
extern void *pvr_icd_resolve(void *instance, const char *name);
static void *dev_proc(VkDevice dev, const char *name) {
    static PFN_vkGetDeviceProcAddr gdpa;
    if (!gdpa) gdpa = (PFN_vkGetDeviceProcAddr)pvr_icd_resolve(NULL, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(dev, name) : NULL;
}

/* ---- per-surface ---------------------------------------------------------------- */

/* Which platform the presentation goes through. A swapchain image is an exportable dma-buf either
 * way; only the "show this image" step differs (wl_buffer vs DRI3 Pixmap + PresentPixmap). */
enum wsi_kind { WSI_WAYLAND = 0, WSI_X11 = 1 };

struct wsi_surface {
    VkSurfaceKHR handle;          /* set to (VkSurfaceKHR)this by the ICD */
    enum wsi_kind kind;
    /* Wayland (kind == WSI_WAYLAND) */
    struct wl_display *display;
    struct wl_surface *surface;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    /* X11 (kind == WSI_X11) */
    xcb_connection_t *c;
    xcb_window_t window;
    uint8_t x_depth, x_bpp;
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
    s->kind = WSI_WAYLAND;
    s->display = ci->display;
    s->surface = ci->surface;
    s->dmabuf = wd ? wd->dmabuf : NULL;
    s->width = 1280; s->height = 720;
    if (!s->dmabuf) WSI_LOG("no zwp_linux_dmabuf_v1 on the display; present will be unavailable");
    *out = (VkSurfaceKHR)s;
    return VK_SUCCESS;
}

/* vkCreateXcbSurfaceKHR / vkCreateXlibSurfaceKHR. Steam is an X11 client under gamescope's Xwayland;
 * a swapchain image here is the same exportable dma-buf, bound to an X Pixmap at present time. */
static VkResult wsi_create_xcb_surface(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *ci,
                                       const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    (void)instance; (void)alloc;
    if (!ci || !ci->connection || !ci->window) return VK_ERROR_INITIALIZATION_FAILED;
    xcb_connection_t *c = (xcb_connection_t *)ci->connection;
    xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(c, &xcb_dri3_id);
    xcb_query_extension_reply_t *present = xcb_get_extension_data(c, &xcb_present_id);
    if (!dri3 || !dri3->present || !present || !present->present) {
        WSI_LOG("X server has no DRI3/Present (%d/%d); present will be unavailable",
                dri3 ? dri3->present : 0, present ? present->present : 0);
    }
    struct wsi_surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->kind = WSI_X11; s->c = c; s->window = ci->window;
    s->x_depth = 24; s->x_bpp = 32; s->width = 1280; s->height = 720;
    *out = (VkSurfaceKHR)s;
    return VK_SUCCESS;
}

static VkResult wsi_create_xlib_surface(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *ci,
                                        const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    /* An Xlib Display* is not an xcb_connection_t; XGetXCBConnection (libX11-xcb) is the only correct
     * bridge. Its result is borrowed — it belongs to the Display — so the surface does not own it. */
    if (!ci || !ci->dpy || !ci->window) return VK_ERROR_INITIALIZATION_FAILED;
    struct { uint32_t sType; const void *pNext; uint32_t flags; xcb_connection_t *connection; xcb_window_t window; } shim = { 0 };
    shim.connection = XGetXCBConnection((Display *)ci->dpy);
    shim.window = (xcb_window_t)ci->window;
    return wsi_create_xcb_surface(instance, (const VkXcbSurfaceCreateInfoKHR *)&shim, alloc, out);
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

struct wsi_swapchain_image { VkImage image; VkDeviceMemory mem; int fd; struct wl_buffer *buffer; xcb_pixmap_t pixmap; };
struct wsi_swapchain {
    VkSwapchainKHR handle;
    VkDevice device;
    struct wsi_surface *surface;
    uint32_t count, next;
    int32_t width, height;
    uint32_t drm_format;
    uint32_t stride;   /* the image's real row pitch, from vkGetImageSubresourceLayout */
    uint64_t serial;   /* PresentPixmap's per-window monotonic serial (X11 present) */
    struct wsi_swapchain_image images[3];
    PFN_vkCreateImage CreateImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkFreeMemory FreeMemory; PFN_vkDestroyImage DestroyImage;
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
    sc->GetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout)dev_proc(device, "vkGetImageSubresourceLayout");
    sc->AllocateMemory = (PFN_vkAllocateMemory)dev_proc(device, "vkAllocateMemory");
    sc->BindImageMemory = (PFN_vkBindImageMemory)dev_proc(device, "vkBindImageMemory");
    sc->FreeMemory = (PFN_vkFreeMemory)dev_proc(device, "vkFreeMemory");
    sc->DestroyImage = (PFN_vkDestroyImage)dev_proc(device, "vkDestroyImage");
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
    uint32_t made = 0;
    for (uint32_t i = 0; i < sc->count; i++) {
        VkResult r = sc->CreateImage(device, &ici, alloc, &sc->images[i].image);
        if (r != VK_SUCCESS) { WSI_LOG("vkCreateImage failed %d", r); goto fail; }
        VkMemoryRequirements mr; sc->GetImageMemoryRequirements(device, sc->images[i].image, &mr);
        VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        ded.pNext = &expo; ded.image = sc->images[i].image;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.pNext = &ded; mai.allocationSize = mr.size; mai.memoryTypeIndex = 0;
        /* The first type the image allows. memoryTypeBits already restricts to types valid for an
         * exportable LINEAR colour image, and this is the selection the dma-buf probes proved
         * exportable on the device. (A host-visible preference needs the physical device, which is
         * not reachable from a VkDevice swapchain create; the vulkaninfo/format dump has the detail.) */
        for (uint32_t t = 0; t < 32; t++) if (mr.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
        r = sc->AllocateMemory(device, &mai, alloc, &sc->images[i].mem);
        if (r != VK_SUCCESS) { WSI_LOG("vkAllocateMemory failed %d", r); goto fail; }
        r = sc->BindImageMemory(device, sc->images[i].image, sc->images[i].mem, 0);
        if (r != VK_SUCCESS) goto fail;
        /* The real pitch, not width*4: a linear image may be padded, and the wl_buffer must be told
         * the pitch the driver actually used or the compositor reads the wrong rows. */
        VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        VkSubresourceLayout layout; memset(&layout, 0, sizeof(layout));
        if (sc->GetImageSubresourceLayout) sc->GetImageSubresourceLayout(device, sc->images[i].image, &sl, &layout);
        sc->stride = (uint32_t)(layout.rowPitch > 0 ? layout.rowPitch : (uint32_t)(sc->width * 4));
        VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        gfi.memory = sc->images[i].mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        sc->images[i].fd = -1;
        if (sc->GetMemoryFdKHR) sc->GetMemoryFdKHR(device, &gfi, &sc->images[i].fd);
        made = i + 1;
    }
    WSI_LOG("swapchain: %u images %dx%d stride %u fmt %d (fd0=%d)", sc->count, sc->width, sc->height,
            sc->stride, ci->imageFormat, sc->images[0].fd);
    *out = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
fail:
    /* Destroy what was made, so a failure to build the swapchain does not leak images, memory and fds. */
    for (uint32_t i = 0; i < made; i++) {
        if (sc->images[i].fd >= 0) { close(sc->images[i].fd); sc->images[i].fd = -1; }
        if (sc->images[i].image) sc->DestroyImage(device, sc->images[i].image, alloc);
        if (sc->images[i].mem) sc->FreeMemory(device, sc->images[i].mem, alloc);
    }
    free(sc);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR sw, const VkAllocationCallbacks *alloc) {
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    if (!sc) return;
    /* The compositor may still hold a wl_buffer on a presented image; drop our reference without
     * waiting, then release everything we own. fds, images and memory are closed/destroyed so a
     * swapchain teardown does not leak one per resize. */
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->images[i].buffer) { wl_buffer_destroy(sc->images[i].buffer); sc->images[i].buffer = NULL; }
        if (sc->images[i].pixmap && sc->surface && sc->surface->kind == WSI_X11)
            xcb_free_pixmap(sc->surface->c, sc->images[i].pixmap);
        if (sc->images[i].fd >= 0) { close(sc->images[i].fd); sc->images[i].fd = -1; }
        if (sc->images[i].image && sc->DestroyImage) sc->DestroyImage(device, sc->images[i].image, alloc);
        if (sc->images[i].mem && sc->FreeMemory) sc->FreeMemory(device, sc->images[i].mem, alloc);
    }
    free(sc);
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

static VkResult wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    (void)queue;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct wsi_swapchain *sc = (struct wsi_swapchain *)pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        if (!sc || idx >= sc->count) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
        struct wsi_surface *surf = sc->surface;
        if (surf->kind == WSI_X11) {
            /* Give the image's dma-buf to Xwayland as a DRI3 Pixmap (once per image) and show it with
             * Present. buffer_from_pixmap is the X->client direction, not needed here. */
            if (sc->images[idx].fd < 0) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
            if (!sc->images[idx].pixmap) {
                xcb_pixmap_t px = xcb_generate_id(surf->c);
                int32_t fds[1] = { sc->images[idx].fd };
                xcb_dri3_pixmap_from_buffers(surf->c, px, surf->window, 1,
                                             (uint16_t)sc->width, (uint16_t)sc->height,
                                             sc->stride, 0, 0, 0, 0, 0, 0, 0,
                                             surf->x_depth, surf->x_bpp, 0 /* LINEAR */, fds);
                sc->images[idx].pixmap = px;
            }
            xcb_present_pixmap(surf->c, surf->window, sc->images[idx].pixmap, ++sc->serial,
                               XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                               XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
            xcb_flush(surf->c);
            if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
            continue;
        }
        if (!surf->dmabuf || sc->images[idx].fd < 0) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
        /* A buffer for this image that the compositor has not released yet: drop our reference
         * (destroying it also frees the binding the release listener would have) so the slot is ours. */
        if (sc->images[idx].buffer) { wl_buffer_destroy(sc->images[idx].buffer); sc->images[idx].buffer = NULL; }
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(surf->dmabuf);
        zwp_linux_buffer_params_v1_add(params, sc->images[idx].fd, 0, 0, sc->stride, 0, 0);
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
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
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
        { "vkCreateXcbSurfaceKHR", (void *)wsi_create_xcb_surface },
        { "vkCreateXlibSurfaceKHR", (void *)wsi_create_xlib_surface },
        { "vkGetPhysicalDeviceXcbPresentationSupportKHR", (void *)wsi_present_support },
        { "vkGetPhysicalDeviceXlibPresentationSupportKHR", (void *)wsi_present_support },
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
