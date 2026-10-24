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
#include <pthread.h>

#define WSI_LOG(fmt, ...) fprintf(stderr, "pvr_wsi[%d]: " fmt, (int)getpid(), ##__VA_ARGS__)

/* Resolve a device entrypoint through the HAL (never the global vkGetDeviceProcAddr). */
extern void *pvr_icd_resolve(void *instance, const char *name);
static void *dev_proc(VkDevice dev, const char *name) {
    static PFN_vkGetDeviceProcAddr gdpa;
    if (!gdpa) gdpa = (PFN_vkGetDeviceProcAddr)pvr_icd_resolve(NULL, "vkGetDeviceProcAddr");
    return gdpa ? gdpa(dev, name) : NULL;
}

/* A release fence the app waits on must actually signal, and an acquire semaphore it makes its
 * vkQueueSubmit wait on must actually be signaled, or its first frame never completes: the submit
 * pends forever on a binary semaphore nobody signaled and vkWaitForFences never returns. The images
 * here are handed out in turn and are always immediately available, so the honest answer is to
 * complete both objects the instant they are returned. Vulkan binary semaphores have no
 * host-side signal entrypoint, so this is done through an empty queue submit -- the HAL's own
 * vkQueueSubmit, because ours must not recurse into itself. */
/* The device-level entries these paths need, taken from the HAL rather than off the global
 * vkGetDeviceProcAddr -- resolving them through pvr_icd_resolve would hand back THIS file's own
 * vkQueueSubmit, and the internal submits below would then recurse into the entrypoint that made
 * them. Filled in when the device is created (see wsi_register_device). */
struct wsi_device_entries {
    VkDevice dev;
    uint32_t queue_family;      /* from the create-info: which family the app's queues come from */
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkGetDeviceQueue GetDeviceQueue;
};
static struct wsi_device_entries g_devs[4];
static int g_ndevs;

/* Called by the ICD when it creates the device, with the HAL entrypoints already resolved. The
 * queue family comes from the first VkDeviceQueueCreateInfo, which is the family an app's graphics
 * queue is on here -- this device has a single general queue family. */
void pvr_wsi_register_device(VkDevice dev, uint32_t queue_family, void *queue_submit, void *create_fence,
                             void *wait_fences, void *destroy_fence, void *get_device_queue) {
    if (g_ndevs >= 4) return;
    struct wsi_device_entries *e = &g_devs[g_ndevs++];
    e->dev = dev;
    e->queue_family = queue_family;
    e->QueueSubmit = (PFN_vkQueueSubmit)queue_submit;
    e->CreateFence = (PFN_vkCreateFence)create_fence;
    e->WaitForFences = (PFN_vkWaitForFences)wait_fences;
    e->DestroyFence = (PFN_vkDestroyFence)destroy_fence;
    e->GetDeviceQueue = (PFN_vkGetDeviceQueue)get_device_queue;
}

static struct wsi_device_entries *wsi_entries(VkDevice dev) {
    for (int i = 0; i < g_ndevs; i++) if (g_devs[i].dev == dev) return &g_devs[i];
    return NULL;
}

/* Fill a freshly created swapchain image through the TRANSFER path, once, before any app work.
 *
 * Why: measured on the Pixel 10 (2026-10-23, probes/pvr_linear_layout_test.c) - an image created
 * LINEAR and exportable, cleared with vkCmdClearColorImage, comes back complete (0 zero rows of
 * 1080); the SAME image cleared by a render pass (loadOp = CLEAR), which is what a game does, comes
 * back with 128 all-zero rows at the top. Real frames showed whole-row zero runs at a 64-row period
 * - 128 = 2 x 64, the same granularity. So the HAL's render-pass clear does not reach the whole
 * image, while its transfer path does.
 *
 * Touching the image here with a transfer clear means the app's first render pass is not the first
 * writer, and every row of the exported dma-buf has been written at least once by a path that is
 * known to cover the image. Best-effort: if any entrypoint is missing the image is simply left
 * as it was, which is the behaviour that existed before this.
 */
static void wsi_clear_image_once(VkDevice device, VkQueue queue, VkImage img, VkFormat fmt,
                                 uint32_t w, uint32_t h) {
    PFN_vkCreateCommandPool CreateCommandPool = (PFN_vkCreateCommandPool)dev_proc(device, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers AllocCB = (PFN_vkAllocateCommandBuffers)dev_proc(device, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer Begin = (PFN_vkBeginCommandBuffer)dev_proc(device, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer End = (PFN_vkEndCommandBuffer)dev_proc(device, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier Barrier = (PFN_vkCmdPipelineBarrier)dev_proc(device, "vkCmdPipelineBarrier");
    PFN_vkCmdClearColorImage Clear = (PFN_vkCmdClearColorImage)dev_proc(device, "vkCmdClearColorImage");
    PFN_vkFreeCommandBuffers FreeCB = (PFN_vkFreeCommandBuffers)dev_proc(device, "vkFreeCommandBuffers");
    PFN_vkDestroyCommandPool DestroyPool = (PFN_vkDestroyCommandPool)dev_proc(device, "vkDestroyCommandPool");
    PFN_vkQueueWaitIdle WaitIdle = (PFN_vkQueueWaitIdle)dev_proc(device, "vkQueueWaitIdle");
    PFN_vkQueueSubmit Submit = (PFN_vkQueueSubmit)dev_proc(device, "vkQueueSubmit");
    struct wsi_device_entries *e = wsi_entries(device);
    WSI_LOG("clear: enter dev=%p pool=%d cb=%d begin=%d end=%d bar=%d clear=%d submit=%d entries=%d qf=%u",
            (void *)device, !!CreateCommandPool, !!AllocCB, !!Begin, !!End, !!Barrier, !!Clear, !!Submit,
            !!e, e ? e->queue_family : 0u);
    /* queue family 0 is a VALID family on this device (there is exactly one), so its value
     * cannot be used to mean "missing" - only the entry itself can. */
    if (!CreateCommandPool || !AllocCB || !Begin || !End || !Barrier || !Clear || !Submit || !e)
        return;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = e->queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (CreateCommandPool(device, &pci, NULL, &pool) != VK_SUCCESS) return;

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (AllocCB(device, &cbai, &cb) != VK_SUCCESS) { if (DestroyPool) DestroyPool(device, pool, NULL); return; }

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    int ok = Begin(cb, &bi) == VK_SUCCESS;
    if (ok) {
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier to_dst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_dst.image = img; to_dst.subresourceRange = rr;
        to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        Barrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &to_dst);
        VkClearColorValue black; memset(&black, 0, sizeof(black));
        Clear(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        /* Hand it back as GENERAL: that is the layout the present path transitions from, so the
         * app's own first use is not surprised by TRANSFER_DST. */
        VkImageMemoryBarrier to_gen = to_dst;
        to_gen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_gen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_gen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_gen.dstAccessMask = 0;
        Barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &to_gen);
        ok = End(cb) == VK_SUCCESS;
    }
    if (ok) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        ok = Submit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok && WaitIdle) WaitIdle(queue);
    }
    if (FreeCB) FreeCB(device, pool, 1, &cb);
    if (DestroyPool) DestroyPool(device, pool, NULL);
    WSI_LOG("create_swapchain: transfer-cleared %ux%u image once (%s)", w, h, ok ? "ok" : "failed");
}


/* The HAL's vkQueueSubmit: signal [sem] and/or complete [fence] on [queue] with an empty submit. */
static int wsi_signal_on_queue(VkDevice dev, VkQueue queue, VkFence fence, VkSemaphore sem) {
    if (!fence && !sem) return 1;
    struct wsi_device_entries *e = wsi_entries(dev);
    if (!e || !e->QueueSubmit || !queue) return 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.signalSemaphoreCount = sem ? 1 : 0;
    si.pSignalSemaphores = sem ? &sem : NULL;
    return e->QueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
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
    struct wsi_display *wd;       /* for the output size, so caps can report a real extent */
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
    int out_w, out_h;   /* the compositor's output mode, so a Wayland surface has a size to report */
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

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                         const char *model, int32_t transform) { (void)data;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)subpixel;(void)make;(void)model;(void)transform; }
static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)o; (void)refresh;
    struct wsi_display *wd = data;
    if ((flags & WL_OUTPUT_MODE_CURRENT) && w > 0 && h > 0) { wd->out_w = w; wd->out_h = h; }
}
static void out_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void out_scale(void *data, struct wl_output *o, int32_t f) { (void)data; (void)o; (void)f; }
static const struct wl_output_listener g_out_listener = { out_geometry, out_mode, out_done, out_scale };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver) {
    struct wsi_display *wd = data;
    if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        wd->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        /* The output's mode is the size a fullscreen surface will be. Tracked so a Wayland surface
         * can report a real currentExtent instead of UINT32_MAX (which makes clients pick 1x1). */
        struct wl_output *o = wl_registry_bind(reg, name, &wl_output_interface, ver < 2 ? ver : 2);
        if (o) wl_output_add_listener(o, &g_out_listener, wd);
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
    s->wd = wd;
    /* No size from the surface itself (a wl_surface is sized by the first buffer committed to it),
     * so fall back to the output mode - a fullscreen surface is that size, and UINT32_MAX would make
     * the client pick 1x1 (see wsi_surface_caps). */
    s->width = (wd && wd->out_w > 0) ? wd->out_w : -1;
    s->height = (wd && wd->out_h > 0) ? wd->out_h : -1;
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
    /* The window's own depth, not an assumed 24: a DRI3 pixmap whose depth differs from its window
     * is a protocol error. depth 24 draws into a 32-bit pixmap, so the bpp is the depth rounded up. */
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(c, ci->window);
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, gc, NULL);
    if (g) {
        s->x_depth = g->depth;
        s->width = g->width;
        s->height = g->height;
        free(g);
    } else {
        s->x_depth = 24;
    }
    s->x_bpp = s->x_depth <= 8 ? 8 : (s->x_depth <= 16 ? 16 : (s->x_depth <= 24 ? 32 : 32));
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
    /* The size of the window this presents into, re-read every time. Reporting a hardcoded extent is
     * worse than reporting none: a real non-UINT32_MAX currentExtent tells the app that exact size is
     * required, so Steam built a 1280x720 swapchain and presented it into the gamescope window (the
     * panel-matched 1600x720 by default) -- a DRI3 pixmap of the wrong size, which present drops or
     * shows garbled. If the window's geometry cannot be read, UINT32_MAX is the spec's "you choose". */
    if (s->kind == WSI_X11 && s->c) {
        xcb_get_geometry_cookie_t gc = xcb_get_geometry(s->c, s->window);
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(s->c, gc, NULL);
        if (g) {
            if (g->width > 0) s->width = g->width;
            if (g->height > 0) s->height = g->height;
            free(g);
        } else {
            s->width = s->height = -1;   /* unreadable: let the app pick, see below */
        }
    } else if (s->kind == WSI_WAYLAND && s->surface) {
        /* wl_surface has no size of its own before a buffer is committed. The output mode is the
         * best available size for a fullscreen surface (the game runs fullscreen in gamescope);
         * without it the client picks 1x1. Re-read each call so a mode change is picked up. */
        if (s->wd && s->wd->out_w > 0 && s->wd->out_h > 0) {
            s->width = s->wd->out_w;
            s->height = s->wd->out_h;
        } else {
            s->width = s->height = -1;
        }
    }
    /* DIAG (Phase 2): log every caps query with surface identity + the returned currentExtent, so the
     * main UI surface and the update-UI surface can be told apart and their extents compared. For an
     * X11 surface also log the window's map state (0=unmapped,1=unviewable,2=viewable). */
    int mapst = -1;
    if (s->kind == WSI_X11 && s->c) {
        xcb_get_window_attributes_cookie_t ac = xcb_get_window_attributes(s->c, s->window);
        xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(s->c, ac, NULL);
        if (a) { mapst = a->map_state; free(a); }
    }
    WSI_LOG("caps: surf=%p kind=%d window=0x%x geom=%dx%d out=%dx%d map=%d -> currentExtent=%ux%u",
            (void *)s, s->kind, (unsigned)s->window, s->width, s->height,
            s->wd ? s->wd->out_w : -1, s->wd ? s->wd->out_h : -1, mapst,
            (s->width <= 0 || s->height <= 0) ? UINT32_MAX : (uint32_t)s->width,
            (s->width <= 0 || s->height <= 0) ? UINT32_MAX : (uint32_t)s->height);
    if (s->width <= 0 || s->height <= 0) {
        caps->currentExtent.width = UINT32_MAX;
        caps->currentExtent.height = UINT32_MAX;
    } else {
        caps->currentExtent.width = (uint32_t)s->width;
        caps->currentExtent.height = (uint32_t)s->height;
    }
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

/* The "2" forms of the two surface queries, from VK_KHR_get_surface_capabilities2. gamescope's WSI
 * layer (libVkLayer_FROG_gamescope_wsi) fetches vkGetPhysicalDeviceSurfaceCapabilities2KHR from the
 * instance and calls it to learn the surface's real extent - the non-2 form is not enough for it, and
 * with no answer it built a 1x1 swapchain and then faulted on a NULL it fetched through its own
 * dispatch table. The loader's trampoline for the 2 form fills the app's pNext chain and calls the
 * ICD only for the base struct, so these delegate to the same helpers the non-2 forms use. */
static VkResult wsi_surface_caps2(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                  VkSurfaceCapabilities2KHR *caps) {
    if (!info || !caps) return VK_ERROR_INITIALIZATION_FAILED;
    return wsi_surface_caps(pd, info->surface, &caps->surfaceCapabilities);
}

static VkResult wsi_surface_formats2(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                     uint32_t *count, VkSurfaceFormat2KHR *formats) {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    if (!formats) {
        uint32_t n = 0;
        VkResult r = wsi_surface_formats(pd, info->surface, &n, NULL);
        *count = n;
        return r;
    }
    /* VkSurfaceFormat2KHR carries sType/pNext before the VkSurfaceFormatKHR, so the array cannot be
     * filled as a plain format array - take the base formats and copy each into its element. */
    uint32_t want = *count;
    VkSurfaceFormatKHR base[8];
    uint32_t n = want < 8 ? want : 8;
    VkResult r = wsi_surface_formats(pd, info->surface, &n, base);
    for (uint32_t i = 0; i < n; i++) formats[i].surfaceFormat = base[i];
    *count = n;
    return r;
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
    uint32_t count;
    uint32_t pending;  /* the image the next acquire hands out */
    int32_t width, height;
    uint32_t drm_format;
    uint32_t stride;   /* the image's real row pitch, from vkGetImageSubresourceLayout */
    uint64_t serial;   /* PresentPixmap's per-window monotonic serial (X11 present) */
    VkQueue queue;     /* the queue the app created its device with: our acquires submit to it */
    /* Serializes the acquire(/present) bookkeeping: the app may own more than one swapchain, and
     * its render thread and a streaming/UI thread can drive two of them at once, both of which
     * land on these same WSI entrypoints. One lock for the whole WSI is coarse but this is a
     * hand-off path, not a hot loop. */
    pthread_mutex_t lock;
    struct wsi_swapchain_image images[3];
    PFN_vkCreateImage CreateImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkFreeMemory FreeMemory; PFN_vkDestroyImage DestroyImage;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR; PFN_vkGetPhysicalDeviceMemoryProperties GetPhysDevMemProps;
    VkPhysicalDeviceMemoryProperties memProps;
};

/* The device lock, held across an acquire so the image index, the release fence and the signal
 * semaphore move together, and across a present so the wait semaphores are honored before the
 * buffer is handed to the compositor. See the note on [struct wsi_swapchain]. */
static pthread_mutex_t wsi_lock = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_init(&sc->lock, NULL);
    sc->device = device; sc->surface = surf; sc->pending = 0;
    /* The queue the app renders on and will present to. It may not have asked the HAL for it yet, so
     * fetch it here and let its own vkGetDeviceQueue return the same one. */
    struct wsi_device_entries *de = wsi_entries(device);
    if (de && de->GetDeviceQueue) de->GetDeviceQueue(device, de->queue_family, 0, &sc->queue);
    sc->width = ci->imageExtent.width; sc->height = ci->imageExtent.height;
    /* DIAG (Phase 2): what the caller asked for and which surface it is. */
    WSI_LOG("create_swapchain: surf=%p kind=%d window=0x%x requested=%ux%u minImg=%u",
            (void *)surf, surf->kind, (unsigned)surf->window,
            ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount);
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
    ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D;
    /* TRANSFER_DST is needed for the creation-time clear below, and it must be in the create info
     * rather than added afterwards: usage is fixed when the image is created. */
    ici.format = ci->imageFormat;
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
        /* Is the allocation big enough for the rows we are about to describe? If it is not, the
         * tail of every described row lies outside the memory the buffer can reach, and a reader
         * gets zeros there. Logged once per image so a session states the geometry it is relying
         * on. Measured on the Pixel 10: 2404x1080 at pitch 16384 needs 17694720 bytes and the HAL
         * allocates 33554432 - ample, so this is not the current fault; it is a guard against a
         * future size/format combination where it would be. */
        {
            VkMemoryRequirements mr2; memset(&mr2, 0, sizeof(mr2));
            sc->GetImageMemoryRequirements(device, sc->images[i].image, &mr2);
            unsigned long long need = (unsigned long long)sc->stride * (unsigned long long)sc->height;
            WSI_LOG("swapchain img%u alloc=%llu need(pitch*h)=%llu offset=%llu pitch=%llu %s", i,
                    (unsigned long long)mr2.size, need, (unsigned long long)layout.offset,
                    (unsigned long long)layout.rowPitch,
                    (mr2.size < need) ? "SHORT - tail of the image is outside the allocation" : "ok");
        }
        VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
        gfi.memory = sc->images[i].mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        sc->images[i].fd = -1;
        if (sc->GetMemoryFdKHR) sc->GetMemoryFdKHR(device, &gfi, &sc->images[i].fd);
        /* Write every row once, through the transfer path that is measured to cover the whole image
         * (see wsi_clear_image_once). The app's first render pass must not be the first writer. */
        wsi_clear_image_once(device, sc->queue, sc->images[i].image, ci->imageFormat,
                             (uint32_t)sc->width, (uint32_t)sc->height);
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
    WSI_LOG("destroy_swapchain: sw=%p %dx%d", (void *)sw, sc->width, sc->height);
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
    pthread_mutex_destroy(&sc->lock);
    free(sc);
}

static VkResult wsi_get_swapchain_images(VkDevice device, VkSwapchainKHR sw, uint32_t *count, VkImage *images) {
    (void)device;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    WSI_LOG("get_images: sw=%p want=%u have=%u -> %s", (void *)sw, images ? *count : 0, sc->count, images ? "fill" : "count");
    if (!images) { *count = sc->count; return VK_SUCCESS; }
    uint32_t n = *count < sc->count ? *count : sc->count;
    for (uint32_t i = 0; i < n; i++) images[i] = sc->images[i].image;
    *count = n;
    return VK_SUCCESS;
}

static VkResult wsi_acquire_next_image(VkDevice device, VkSwapchainKHR sw, uint64_t timeout,
                                       VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)timeout;
    WSI_LOG("acquire: enter sw=%p sem=%p fence=%p", (void *)sw, (void *)sem, (void *)fence);
    if (!sw || !index) return VK_ERROR_INITIALIZATION_FAILED;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    pthread_mutex_lock(&sc->lock);
    uint32_t idx = sc->pending;
    sc->pending = (idx + 1) % sc->count;
    int ok = wsi_signal_on_queue(device, sc->queue, fence, sem);
    pthread_mutex_unlock(&sc->lock);
    if (!ok && (sem || fence)) {
        /* Say so rather than hand back an image the app will wait on forever. */
        WSI_LOG("acquire: no HAL vkQueueSubmit for %p; the frame would hang", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *index = idx;
    WSI_LOG("acquire: sw=%p -> image %u", (void *)sw, idx);
    return VK_SUCCESS;
}

/* vkAcquireNextImage2KHR, from VK_KHR_swapchain. gamescope's WSI layer implements
 * vkAcquireNextImageKHR by building a VkAcquireNextImageInfoKHR and calling this entry through its
 * device dispatch table; when the ICD did not provide it, the slot was NULL and the layer called
 * through it (si_addr=0, pc=0, inside AcquireNextImageKHR). The extra deviceMask/swapchain pair in
 * the info struct is the only difference from the 1KHR form, which this WSI already serves. */
static VkResult wsi_acquire_next_image2(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *index) {
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    return wsi_acquire_next_image(device, info->swapchain, info->timeout,
                                  info->semaphore, info->fence, index);
}

/* ---- present: dmabuf fd -> wl_buffer -> attach -> commit ------------------------ */

/* Wait for a binary semaphore the app signaled through vkQueueSubmit. There is no host-side
 * vkWaitSemaphores for a binary semaphore (that entrypoint is for timelines), so the wait is
 * expressed as a queue operation: an empty submit that takes the semaphore as a WAIT and a fresh
 * fence, then a wait on that fence. The timeout exists only so a semaphore that never signals is
 * reported instead of hanging the session. */
static VkResult wsi_wait_binary(VkDevice dev, VkQueue queue, VkSemaphore sem) {
    struct wsi_device_entries *e = wsi_entries(dev);
    if (!e || !e->QueueSubmit || !e->CreateFence || !e->WaitForFences || !e->DestroyFence) {
        WSI_LOG("present: no HAL fence entries for %p; not waiting on the semaphore", (void *)dev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f = VK_NULL_HANDLE;
    VkResult r = e->CreateFence(dev, &fci, NULL, &f);
    if (r != VK_SUCCESS) return r;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sem; si.pWaitDstStageMask = &stage;
    r = e->QueueSubmit(queue, 1, &si, f);
    if (r == VK_SUCCESS) r = e->WaitForFences(dev, 1, &f, VK_TRUE, 1000000000ull /* 1s */);
    e->DestroyFence(dev, f, NULL);
    return r;
}

static VkResult wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    (void)queue;
    WSI_LOG("present: enter count=%u sw0=%p image0=%u", pi->swapchainCount,
            pi->swapchainCount ? (void *)pi->pSwapchains[0] : NULL,
            pi->swapchainCount ? pi->pImageIndices[0] : 0);
    pthread_mutex_lock(&wsi_lock);
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct wsi_swapchain *sc = (struct wsi_swapchain *)pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        /* What the frame was rendered behind: the app makes the queue's rendering wait on a
         * semaphore and presents with it here. The image is about to be read by the compositor, so
         * wait for that work to finish first. Waiting with a timeout keeps a semaphore that never
         * signals from wedging the session outright. It must happen before the early-outs below, or
         * a present that fails for any other reason would leave the app's frame outstanding. */
        if (sc && pi->pWaitSemaphores && i < pi->waitSemaphoreCount) {
            VkSemaphore ws = pi->pWaitSemaphores[i];
            if (ws) {
                VkResult w = wsi_wait_binary(sc->device, queue, ws);
                if (w != VK_SUCCESS) WSI_LOG("present: wait on the render semaphore failed %d", w);
            }
        }
        if (!sc || idx >= sc->count) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
        struct wsi_surface *surf = sc->surface;
        if (surf->kind == WSI_X11) {
            /* Give the image's dma-buf to Xwayland as a DRI3 Pixmap (once per image) and show it with
             * Present. buffer_from_pixmap is the X->client direction, not needed here. */
            static int x11_logged;
            if (x11_logged < 40) { x11_logged++;
                WSI_LOG("X11 present: window=0x%x %dx%d stride=%u depth=%u bpp=%u fd=%d (DRI3 PixmapFromBuffers)",
                        (unsigned)surf->window, sc->width, sc->height, sc->stride, surf->x_depth, surf->x_bpp, sc->images[idx].fd); }
            if (sc->images[idx].fd < 0) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
            if (!sc->images[idx].pixmap) {
                xcb_pixmap_t px = xcb_generate_id(surf->c);
                int32_t fds[1] = { sc->images[idx].fd };
                xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffers_checked(surf->c, px, surf->window, 1,
                                             (uint16_t)sc->width, (uint16_t)sc->height,
                                             sc->stride, 0, 0, 0, 0, 0, 0, 0,
                                             surf->x_depth, surf->x_bpp, 0 /* LINEAR */, fds);
                /* Checked: X errors are async, so the BadAlloc seen in the log could be this very
                 * request. The error (or its absence) says which. */
                xcb_generic_error_t *err = xcb_request_check(surf->c, ck);
                if (err) {
                    WSI_LOG("X11 present: DRI3 PixmapFromBuffers failed: error_code=%u major=%u minor=%u "
                            "(window=0x%x %dx%d stride=%u depth=%u)",
                            err->error_code, err->major_code, err->minor_code,
                            (unsigned)surf->window, sc->width, sc->height, sc->stride, surf->x_depth);
                    free(err);
                }
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
        /* One wl_buffer per swapchain image, reused for that image's whole life. A wl_buffer
         * describes an fd, a size and a format, all fixed once the image exists, so minting a new
         * one per present only leaks pool entries in the compositor. Measured before this fix:
         * presents=24 bufs_created=24 releases=0 - then gamescope stopped committing frames
         * because every buffer it had was held. */
        struct wl_buffer *buf = sc->images[idx].buffer;
        if (!buf) {
            struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(surf->dmabuf);
            zwp_linux_buffer_params_v1_add(params, sc->images[idx].fd, 0, 0, sc->stride, 0, 0);
            buf = zwp_linux_buffer_params_v1_create_immed(params, sc->width, sc->height, sc->drm_format, 0 /* LINEAR modifier */);
            zwp_linux_buffer_params_v1_destroy(params);
            if (!buf) { if (pi->pResults) pi->pResults[i] = VK_ERROR_OUT_OF_HOST_MEMORY; continue; }
            struct wsi_buffer_binding *b = calloc(1, sizeof(*b)); b->sc = sc; b->index = idx;
            wl_buffer_add_listener(buf, &g_buffer_listener, b);
            sc->images[idx].buffer = buf;
        }
        wl_surface_attach(surf->surface, buf, 0, 0);
        wl_surface_damage_buffer(surf->surface, 0, 0, sc->width, sc->height);
        wl_surface_commit(surf->surface);
        wl_display_flush(surf->display);
        { static int wl_logged; if (wl_logged < 20) { wl_logged++;
            WSI_LOG("Wayland present: %dx%d stride=%u fmt=%u fd=%d buffer=%p (wl_buffer attach+commit)",
                    sc->width, sc->height, sc->stride, sc->drm_format, sc->images[idx].fd, (void *)buf); } }
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    pthread_mutex_unlock(&wsi_lock);
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
        /* VK_KHR_get_surface_capabilities2: gamescope's WSI layer uses the 2 forms. */
        { "vkGetPhysicalDeviceSurfaceCapabilities2KHR", (void *)wsi_surface_caps2 },
        { "vkGetPhysicalDeviceSurfaceFormats2KHR", (void *)wsi_surface_formats2 },
        { "vkGetPhysicalDeviceWaylandPresentationSupportKHR", (void *)wsi_present_support },
        { "vkGetPhysicalDeviceSurfaceSupportKHR", (void *)wsi_present_support },
        { "vkCreateSwapchainKHR", (void *)wsi_create_swapchain },
        { "vkDestroySwapchainKHR", (void *)wsi_destroy_swapchain },
        { "vkGetSwapchainImagesKHR", (void *)wsi_get_swapchain_images },
        { "vkAcquireNextImageKHR", (void *)wsi_acquire_next_image },
        { "vkAcquireNextImage2KHR", (void *)wsi_acquire_next_image2 },
        { "vkQueuePresentKHR", (void *)wsi_queue_present },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(*t); i++) if (!strcmp(name, t[i].n)) return t[i].p;
    return NULL;
}
