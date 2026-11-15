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
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>

#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
extern const char *pvr_proc_cmd(void);
static int g_wsi_log_fd = -2;
static pthread_mutex_t g_wsi_log_mu = PTHREAD_MUTEX_INITIALIZER;
static void wsi_log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wsi_log_write(const char *fmt, ...) {
    char buf[1024];
    const char *cmd = pvr_proc_cmd();
    int prefix = snprintf(buf, sizeof(buf), "pvr_wsi[%d:%.64s]: ", (int)getpid(), cmd ? cmd : "?");
    if (prefix < 0) prefix = 0;
    if ((size_t)prefix >= sizeof(buf)) prefix = (int)sizeof(buf) - 1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + prefix, sizeof(buf) - (size_t)prefix, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    size_t total = (size_t)prefix + (size_t)n;
    if (total >= sizeof(buf) - 1) total = sizeof(buf) - 2;
    buf[total++] = '\n';
    buf[total] = '\0';
    fwrite(buf, 1, total, stderr);
    fflush(stderr);
    pthread_mutex_lock(&g_wsi_log_mu);
    if (g_wsi_log_fd == -2) {
        g_wsi_log_fd = open("/tmp/pvr_wsi.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (g_wsi_log_fd < 0) g_wsi_log_fd = -1;
    }
    int fd = g_wsi_log_fd;
    pthread_mutex_unlock(&g_wsi_log_mu);
    if (fd >= 0) {
        ssize_t w = write(fd, buf, total);
        (void)w;
    }
}
#define WSI_LOG(fmt, ...) wsi_log_write(fmt, ##__VA_ARGS__)
static int wsi_verbose_log(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("PVR_WSI_VERBOSE"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on;
}

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
typedef VkResult (VKAPI_PTR *PFN_pvr_QueueSubmit2)(VkQueue, uint32_t, const void *, VkFence);
typedef void (VKAPI_PTR *PFN_pvr_GetDeviceQueue2)(VkDevice, const void *, VkQueue *);

struct wsi_device_entries {
    int in_use;
    int mu_init;
    VkDevice dev;
    uint32_t queue_family;      /* from the create-info: which family the app's queues come from */
    /* Recursive per-device mutex serializing all HAL queue submits & wait-idles (PowerVR HAL's
     * vkQueueSubmit is not thread-safe across zcfq0 / present / acquire threads). Lock order:
     * wsi_lock -> sc->lock -> queue_mu. Unlock queue_mu BEFORE WaitForFences. */
    pthread_mutex_t queue_mu;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_pvr_QueueSubmit2 QueueSubmit2;
    PFN_pvr_QueueSubmit2 QueueSubmit2KHR;
    PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkDeviceWaitIdle DeviceWaitIdle;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkDestroyFence DestroyFence;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_pvr_GetDeviceQueue2 GetDeviceQueue2;
    PFN_vkDestroyDevice DestroyDevice;
};
#define WSI_MAX_DEVS 64
#define WSI_MAX_QUEUES 256
struct wsi_queue_map {
    int in_use;
    VkQueue queue;
    VkDevice dev;
    struct wsi_device_entries *entry;
};
static struct wsi_device_entries g_devs[WSI_MAX_DEVS];
static struct wsi_queue_map g_queues[WSI_MAX_QUEUES];
static int g_ndevs;
static pthread_mutex_t g_devs_mu = PTHREAD_MUTEX_INITIALIZER;

static struct wsi_device_entries *wsi_find_dev_locked(VkDevice dev) {
    for (int i = 0; i < WSI_MAX_DEVS; i++) {
        if (g_devs[i].in_use && g_devs[i].dev == dev) return &g_devs[i];
    }
    return NULL;
}

/* Called by the ICD when it creates the device, with the HAL entrypoints already resolved. The
 * queue family comes from the first VkDeviceQueueCreateInfo, which is the family an app's graphics
 * queue is on here -- this device has a single general queue family. */
void pvr_wsi_register_device(VkDevice dev, uint32_t queue_family, void *queue_submit, void *create_fence,
                             void *wait_fences, void *destroy_fence, void *get_device_queue,
                             void *queue_submit2, void *queue_submit2_khr, void *queue_wait_idle,
                             void *device_wait_idle, void *get_device_queue2, void *destroy_device) {
    pthread_mutex_lock(&g_devs_mu);
    struct wsi_device_entries *e = wsi_find_dev_locked(dev);
    if (!e) {
        for (int i = 0; i < WSI_MAX_DEVS; i++) {
            if (!g_devs[i].in_use) { e = &g_devs[i]; break; }
        }
    }
    if (!e) {
        e = &g_devs[g_ndevs % WSI_MAX_DEVS];
    }
    g_ndevs++;
    if (!e->mu_init) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&e->queue_mu, &attr);
        pthread_mutexattr_destroy(&attr);
        e->mu_init = 1;
    }
    e->in_use = 1;
    e->dev = dev;
    e->queue_family = queue_family;
    e->QueueSubmit = (PFN_vkQueueSubmit)queue_submit;
    e->QueueSubmit2 = (PFN_pvr_QueueSubmit2)queue_submit2;
    e->QueueSubmit2KHR = (PFN_pvr_QueueSubmit2)queue_submit2_khr;
    e->QueueWaitIdle = (PFN_vkQueueWaitIdle)queue_wait_idle;
    e->DeviceWaitIdle = (PFN_vkDeviceWaitIdle)device_wait_idle;
    e->CreateFence = (PFN_vkCreateFence)create_fence;
    e->WaitForFences = (PFN_vkWaitForFences)wait_fences;
    e->DestroyFence = (PFN_vkDestroyFence)destroy_fence;
    e->GetDeviceQueue = (PFN_vkGetDeviceQueue)get_device_queue;
    e->GetDeviceQueue2 = (PFN_pvr_GetDeviceQueue2)get_device_queue2;
    e->DestroyDevice = (PFN_vkDestroyDevice)destroy_device;
    pthread_mutex_unlock(&g_devs_mu);
}

void pvr_wsi_note_queue(VkDevice dev, VkQueue queue) {
    if (!dev || !queue) return;
    pthread_mutex_lock(&g_devs_mu);
    struct wsi_device_entries *e = wsi_find_dev_locked(dev);
    int slot = -1;
    for (int i = 0; i < WSI_MAX_QUEUES; i++) {
        if (g_queues[i].in_use && g_queues[i].queue == queue) { slot = i; break; }
        if (!g_queues[i].in_use && slot < 0) slot = i;
    }
    if (slot >= 0) {
        g_queues[slot].in_use = 1;
        g_queues[slot].queue = queue;
        g_queues[slot].dev = dev;
        g_queues[slot].entry = e;
    }
    pthread_mutex_unlock(&g_devs_mu);
}

void pvr_wsi_unregister_device(VkDevice dev) {
    if (!dev) return;
    pthread_mutex_lock(&g_devs_mu);
    for (int i = 0; i < WSI_MAX_QUEUES; i++) {
        if (g_queues[i].in_use && g_queues[i].dev == dev) {
            g_queues[i].in_use = 0;
            g_queues[i].queue = VK_NULL_HANDLE;
            g_queues[i].dev = VK_NULL_HANDLE;
            g_queues[i].entry = NULL;
        }
    }
    struct wsi_device_entries *e = wsi_find_dev_locked(dev);
    if (e) {
        e->in_use = 0;
        e->dev = VK_NULL_HANDLE;
        /* Keep queue_mu initialized so any in-flight caller holding its pointer stays valid. */
    }
    pthread_mutex_unlock(&g_devs_mu);
}

static struct wsi_device_entries *wsi_entries(VkDevice dev) {
    pthread_mutex_lock(&g_devs_mu);
    struct wsi_device_entries *e = wsi_find_dev_locked(dev);
    pthread_mutex_unlock(&g_devs_mu);
    return e;
}

static int wsi_copy_for_dev(VkDevice dev, struct wsi_device_entries *out, pthread_mutex_t **out_mu) {
    pthread_mutex_lock(&g_devs_mu);
    struct wsi_device_entries *e = wsi_find_dev_locked(dev);
    if (!e) {
        pthread_mutex_unlock(&g_devs_mu);
        return 0;
    }
    *out = *e;
    *out_mu = &e->queue_mu;
    pthread_mutex_unlock(&g_devs_mu);
    return 1;
}

static int wsi_copy_for_queue(VkQueue queue, struct wsi_device_entries *out, pthread_mutex_t **out_mu) {
    pthread_mutex_lock(&g_devs_mu);
    struct wsi_device_entries *e = NULL;
    for (int i = 0; i < WSI_MAX_QUEUES; i++) {
        if (g_queues[i].in_use && g_queues[i].queue == queue) {
            e = g_queues[i].entry;
            if (!e || !e->in_use) e = wsi_find_dev_locked(g_queues[i].dev);
            break;
        }
    }
    if (!e) {
        /* Fallback only if exactly one live device exists. */
        int active = 0;
        struct wsi_device_entries *only = NULL;
        for (int i = 0; i < WSI_MAX_DEVS; i++) {
            if (g_devs[i].in_use) { active++; only = &g_devs[i]; }
        }
        if (active == 1) e = only;
    }
    if (!e) {
        pthread_mutex_unlock(&g_devs_mu);
        return 0;
    }
    *out = *e;
    *out_mu = &e->queue_mu;
    pthread_mutex_unlock(&g_devs_mu);
    return 1;
}

VkResult VKAPI_PTR pvr_wsi_queue_submit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    if (!wsi_copy_for_queue(queue, &ec, &mu) || !ec.QueueSubmit) {
        WSI_LOG("queue_submit: unknown queue=%p", (void *)queue);
        return VK_ERROR_DEVICE_LOST;
    }
    pthread_mutex_lock(mu);
    VkResult r = ec.QueueSubmit(queue, submitCount, pSubmits, fence);
    pthread_mutex_unlock(mu);
    return r;
}

VkResult VKAPI_PTR pvr_wsi_queue_submit2(VkQueue queue, uint32_t submitCount, const void *pSubmits, VkFence fence) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    if (!wsi_copy_for_queue(queue, &ec, &mu) || !ec.QueueSubmit2) {
        WSI_LOG("queue_submit2: missing HAL entry for queue=%p", (void *)queue);
        return VK_ERROR_DEVICE_LOST;
    }
    pthread_mutex_lock(mu);
    VkResult r = ec.QueueSubmit2(queue, submitCount, pSubmits, fence);
    pthread_mutex_unlock(mu);
    return r;
}

VkResult VKAPI_PTR pvr_wsi_queue_submit2_khr(VkQueue queue, uint32_t submitCount, const void *pSubmits, VkFence fence) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    if (!wsi_copy_for_queue(queue, &ec, &mu)) return VK_ERROR_DEVICE_LOST;
    PFN_pvr_QueueSubmit2 fn = ec.QueueSubmit2KHR ? ec.QueueSubmit2KHR : ec.QueueSubmit2;
    if (!fn) return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(mu);
    VkResult r = fn(queue, submitCount, pSubmits, fence);
    pthread_mutex_unlock(mu);
    return r;
}

VkResult VKAPI_PTR pvr_wsi_queue_wait_idle(VkQueue queue) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    if (!wsi_copy_for_queue(queue, &ec, &mu) || !ec.QueueWaitIdle) return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(mu);
    VkResult r = ec.QueueWaitIdle(queue);
    pthread_mutex_unlock(mu);
    return r;
}

VkResult VKAPI_PTR pvr_wsi_device_wait_idle(VkDevice dev) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    if (!wsi_copy_for_dev(dev, &ec, &mu) || !ec.DeviceWaitIdle) return VK_ERROR_DEVICE_LOST;
    pthread_mutex_lock(mu);
    VkResult r = ec.DeviceWaitIdle(dev);
    pthread_mutex_unlock(mu);
    return r;
}

void VKAPI_PTR pvr_wsi_get_device_queue(VkDevice dev, uint32_t qf, uint32_t qi, VkQueue *pQueue) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    PFN_vkGetDeviceQueue fn = wsi_copy_for_dev(dev, &ec, &mu) ? ec.GetDeviceQueue : (PFN_vkGetDeviceQueue)dev_proc(dev, "vkGetDeviceQueue");
    if (fn) fn(dev, qf, qi, pQueue);
    if (pQueue && *pQueue) pvr_wsi_note_queue(dev, *pQueue);
}

void VKAPI_PTR pvr_wsi_get_device_queue2(VkDevice dev, const void *pQueueInfo, VkQueue *pQueue) {
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    PFN_pvr_GetDeviceQueue2 fn = wsi_copy_for_dev(dev, &ec, &mu) ? ec.GetDeviceQueue2 : (PFN_pvr_GetDeviceQueue2)dev_proc(dev, "vkGetDeviceQueue2");
    if (fn) fn(dev, pQueueInfo, pQueue);
    if (pQueue && *pQueue) pvr_wsi_note_queue(dev, *pQueue);
}

void VKAPI_PTR pvr_wsi_destroy_device(VkDevice dev, const VkAllocationCallbacks *alloc) {
    if (!dev) return;
    struct wsi_device_entries ec;
    pthread_mutex_t *mu = NULL;
    PFN_vkDestroyDevice fn = wsi_copy_for_dev(dev, &ec, &mu) ? ec.DestroyDevice : (PFN_vkDestroyDevice)dev_proc(dev, "vkDestroyDevice");
    if (fn) fn(dev, alloc);
    pvr_wsi_unregister_device(dev);
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
    struct wsi_device_entries ecopy;
    pthread_mutex_t *qmu = NULL;
    int have_dev = wsi_copy_for_dev(device, &ecopy, &qmu);
    WSI_LOG("clear: enter dev=%p pool=%d cb=%d begin=%d end=%d bar=%d clear=%d submit=%d entries=%d qf=%u",
            (void *)device, !!CreateCommandPool, !!AllocCB, !!Begin, !!End, !!Barrier, !!Clear,
            have_dev ? !!ecopy.QueueSubmit : 0, have_dev, have_dev ? ecopy.queue_family : 0u);
    /* queue family 0 is a VALID family on this device (there is exactly one), so its value
     * cannot be used to mean "missing" - only the entry itself can. */
    if (!CreateCommandPool || !AllocCB || !Begin || !End || !Barrier || !Clear || !have_dev || !ecopy.QueueSubmit)
        return;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = ecopy.queue_family;
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
        VkFence f = VK_NULL_HANDLE;
        if (ecopy.CreateFence) {
            VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            ecopy.CreateFence(device, &fci, NULL, &f);
        }
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        pthread_mutex_lock(qmu);
        ok = ecopy.QueueSubmit(queue, 1, &si, f) == VK_SUCCESS;
        pthread_mutex_unlock(qmu);
        if (ok && f != VK_NULL_HANDLE && ecopy.WaitForFences) {
            ok = ecopy.WaitForFences(device, 1, &f, VK_TRUE, 1000000000ull) == VK_SUCCESS;
        }
        if (f != VK_NULL_HANDLE && ecopy.DestroyFence) ecopy.DestroyFence(device, f, NULL);
    }
    if (FreeCB) FreeCB(device, pool, 1, &cb);
    if (DestroyPool) DestroyPool(device, pool, NULL);
    WSI_LOG("create_swapchain: transfer-cleared %ux%u image once (%s)", w, h, ok ? "ok" : "failed");
}


/* The HAL's vkQueueSubmit: signal [sem] and/or complete [fence] on [queue] with an empty submit. */
static int wsi_signal_on_queue(VkDevice dev, VkQueue queue, VkFence fence, VkSemaphore sem) {
    if (!fence && !sem) return 1;
    struct wsi_device_entries ecopy;
    pthread_mutex_t *qmu = NULL;
    if (!wsi_copy_for_dev(dev, &ecopy, &qmu) || !ecopy.QueueSubmit || !queue) return 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.signalSemaphoreCount = sem ? 1 : 0;
    si.pSignalSemaphores = sem ? &sem : NULL;
    pthread_mutex_lock(qmu);
    VkResult r = ecopy.QueueSubmit(queue, 1, &si, fence);
    pthread_mutex_unlock(qmu);
    return r == VK_SUCCESS;
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
    xcb_gcontext_t gc;
    uint8_t x_depth, x_bpp;
    int has_dri3, has_present;
    /* Filled on the first caps query for this surface: the physical device and its memory types, so
     * the present-time readback can find a host-visible type (the HAL exposes none that is also
     * coherent, and the swapchain used to keep an all-zero copy of this struct). */
    void *pd; VkPhysicalDeviceMemoryProperties memProps; int have_mem;
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
static pthread_mutex_t g_display_mu = PTHREAD_MUTEX_INITIALIZER;

static struct wsi_display *wsi_for_display(struct wl_display *d) {
    pthread_mutex_lock(&g_display_mu);
    for (int i = 0; i < g_ndisplays; i++) {
        if (g_displays[i].display == d) {
            pthread_mutex_unlock(&g_display_mu);
            return &g_displays[i];
        }
    }
    if (g_ndisplays >= 8) {
        pthread_mutex_unlock(&g_display_mu);
        return NULL;
    }
    struct wsi_display *wd = &g_displays[g_ndisplays++];
    wd->display = d;
    wd->registry = wl_display_get_registry(d);
    wl_registry_add_listener(wd->registry, &g_reg_listener, wd);
    wl_display_roundtrip(d);   /* fire the globals */
    wl_display_roundtrip(d);   /* dmabuf format events */
    pthread_mutex_unlock(&g_display_mu);
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
    const xcb_query_extension_reply_t *dri3 = xcb_get_extension_data(c, &xcb_dri3_id);
    const xcb_query_extension_reply_t *present = xcb_get_extension_data(c, &xcb_present_id);
    if (!dri3 || !dri3->present || !present || !present->present) {
        WSI_LOG("X server has no DRI3/Present (%d/%d); present will be unavailable",
                dri3 ? dri3->present : 0, present ? present->present : 0);
    }
    struct wsi_surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->kind = WSI_X11; s->c = c; s->window = ci->window;
    s->has_dri3 = (dri3 && dri3->present) ? 1 : 0;
    s->has_present = (present && present->present) ? 1 : 0;
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
    struct wsi_surface *s = (struct wsi_surface *)surface;
    if (s && s->kind == WSI_X11 && s->gc && s->c && !xcb_connection_has_error(s->c))
        xcb_free_gc(s->c, s->gc);
    free(s);
}

/* ---- surface queries ------------------------------------------------------------ */

static VkPhysicalDeviceMemoryProperties g_cached_memProps;
static int g_have_cached_mem = 0;
static pthread_mutex_t g_memprops_mu = PTHREAD_MUTEX_INITIALIZER;

static VkResult wsi_surface_caps(VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps) {
    (void)pd;
    struct wsi_surface *s = (struct wsi_surface *)surface;
    memset(caps, 0, sizeof(*caps));
    { extern int pvr_icd_mem_props(void *, void *);
      if (pd && !s->have_mem) {
          if (pvr_icd_mem_props((void *)pd, &s->memProps) == 0 && s->memProps.memoryTypeCount > 0) {
              s->pd = (void *)pd;
              s->have_mem = 1;
              pthread_mutex_lock(&g_memprops_mu);
              g_cached_memProps = s->memProps;
              g_have_cached_mem = 1;
              pthread_mutex_unlock(&g_memprops_mu);
          }
      } }
    caps->minImageCount = 3;
    caps->maxImageCount = 4;
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
        /* Per VK_KHR_wayland_surface, a Wayland surface does not dictate a fixed extent;
         * reporting UINT32_MAX lets the client (e.g. Zink/CEF) choose the swapchain size that
         * matches its actual buffer instead of forcing the compositor's output mode. */
        s->width = s->height = -1;
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
    { static int cap_n;
      if (cap_n < 16 || wsi_verbose_log()) {
          cap_n++;
          WSI_LOG("caps: surf=%p kind=%d window=0x%x geom=%dx%d out=%dx%d map=%d -> currentExtent=%ux%u",
                  (void *)s, s->kind, (unsigned)s->window, s->width, s->height,
                  s->wd ? s->wd->out_w : -1, s->wd ? s->wd->out_h : -1, mapst,
                  (s->width <= 0 || s->height <= 0) ? UINT32_MAX : (uint32_t)s->width,
                  (s->width <= 0 || s->height <= 0) ? UINT32_MAX : (uint32_t)s->height);
      } }
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
    static const VkPresentModeKHR m[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    if (!modes) { *count = 3; return VK_SUCCESS; }
    uint32_t n = *count < 3 ? *count : 3;
    memcpy(modes, m, n * sizeof(*m));
    *count = n;
    return (*count < 3) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- swapchain ------------------------------------------------------------------ */

struct wsi_swapchain_image {
    VkImage image;              /* LINEAR, dma-buf exportable: what gamescope imports */
    VkDeviceMemory mem;
    int fd; struct wl_buffer *buffer; xcb_pixmap_t pixmap;
    int busy;                   /* held by Wayland compositor until wl_buffer.release fires */
    int acquired;               /* checked out by AcquireNextImage until QueuePresent */
    /* What the app actually renders into. The HAL's render-pass writes into a LINEAR image are the
     * incomplete path (see wsi_clear_image_once), so the visible image is filled by a copy from this
     * one at present time rather than being drawn into directly. */
    VkImage render; VkDeviceMemory render_mem;
};
struct wsi_swapchain {
    VkSwapchainKHR handle;
    VkDevice device;
    struct wsi_surface *surface;
    struct wl_event_queue *wl_queue;
    struct zwp_linux_dmabuf_v1 *dmabuf_wrap;
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
    struct wsi_swapchain_image images[4];
    PFN_vkCreateImage CreateImage; PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkGetImageSubresourceLayout GetImageSubresourceLayout;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkFreeMemory FreeMemory; PFN_vkDestroyImage DestroyImage;
    PFN_vkGetMemoryFdKHR GetMemoryFdKHR; PFN_vkGetPhysicalDeviceMemoryProperties GetPhysDevMemProps;
    VkPhysicalDeviceMemoryProperties memProps;
    /* Persistent present-time copy resources (created on first use): allocating a pool, a command
     * buffer and a fence per present would add churn to every frame, which is the kind of stutter
     * that reads as flashing. */
    VkCommandPool blit_pool; VkCommandBuffer blit_cb; VkFence blit_fence;
    PFN_vkBeginCommandBuffer blit_Begin;
    PFN_vkEndCommandBuffer blit_End;
    PFN_vkCmdPipelineBarrier blit_Barrier;
    PFN_vkCmdCopyImage blit_Copy;
    PFN_vkResetCommandBuffer blit_Reset;
    PFN_vkResetFences blit_ResetFences;
};

/* The device lock, held across an acquire so the image index, the release fence and the signal
 * semaphore move together, and across a present so the wait semaphores are honored before the
 * buffer is handed to the compositor. See the note on [struct wsi_swapchain]. */
static pthread_mutex_t wsi_lock = PTHREAD_MUTEX_INITIALIZER;

/* Each swapchain image keeps one wl_buffer for the lifetime of the swapchain and is destroyed
 * once in wsi_destroy_swapchain; destroying/freeing inside buffer_release caused double-frees
 * and use-after-free on sc when events drained after reuse or swapchain teardown. Using a
 * dedicated wl_event_queue per swapchain lets us clear img->busy safely under sc->lock without
 * touching the app's own Wayland queue. */
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    struct wsi_swapchain_image *img = (struct wsi_swapchain_image *)data;
    if (img) img->busy = 0;
}
static const struct wl_buffer_listener g_buffer_listener = { buffer_release };

static void wsi_drain_wl_queue(struct wsi_swapchain *sc, int wait_ms) {
    if (!sc || !sc->wl_queue || !sc->surface || !sc->surface->display) return;
    struct wl_display *d = sc->surface->display;
    if (wl_display_dispatch_queue_pending(d, sc->wl_queue) > 0 || wait_ms <= 0) return;
    for (int tries = 0; tries < 4; tries++) {
        if (wl_display_prepare_read_queue(d, sc->wl_queue) == 0) {
            wl_display_flush(d);
            struct pollfd pfd;
            pfd.fd = wl_display_get_fd(d);
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (pfd.fd >= 0 && poll(&pfd, 1, wait_ms) > 0 && (pfd.revents & POLLIN)) {
                wl_display_read_events(d);
            } else {
                wl_display_cancel_read(d);
            }
            wl_display_dispatch_queue_pending(d, sc->wl_queue);
            break;
        }
        if (wl_display_dispatch_queue_pending(d, sc->wl_queue) <= 0) break;
    }
}

static VkResult wsi_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                     const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    struct wsi_surface *surf = (struct wsi_surface *)ci->surface;
    struct wsi_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&sc->lock, NULL);
    for (uint32_t i = 0; i < 4; i++) sc->images[i].fd = -1;
    sc->device = device; sc->surface = surf; sc->pending = 0;
    if (surf && surf->kind == WSI_WAYLAND && surf->display && surf->dmabuf) {
        sc->wl_queue = wl_display_create_queue(surf->display);
        if (sc->wl_queue) {
            sc->dmabuf_wrap = (struct zwp_linux_dmabuf_v1 *)wl_proxy_create_wrapper(surf->dmabuf);
            if (sc->dmabuf_wrap) wl_proxy_set_queue((struct wl_proxy *)sc->dmabuf_wrap, sc->wl_queue);
        }
    }
    /* The queue the app renders on and will present to. It may not have asked the HAL for it yet, so
     * fetch it here and let its own vkGetDeviceQueue return the same one. */
    struct wsi_device_entries *de = wsi_entries(device);
    if (de && de->GetDeviceQueue) {
        de->GetDeviceQueue(device, de->queue_family, 0, &sc->queue);
        if (sc->queue) pvr_wsi_note_queue(device, sc->queue);
    }
    sc->width = ci->imageExtent.width; sc->height = ci->imageExtent.height;
    /* DIAG (Phase 2): what the caller asked for and which surface it is. */
    WSI_LOG("create_swapchain: surf=%p kind=%d window=0x%x requested=%ux%u minImg=%u",
            (void *)surf, surf->kind, (unsigned)surf->window,
            ci->imageExtent.width, ci->imageExtent.height, ci->minImageCount);
    sc->drm_format = drm_format_for(ci->imageFormat);
    sc->count = ci->minImageCount > 4 ? 4 : (ci->minImageCount < 2 ? 2 : ci->minImageCount);
    sc->CreateImage = (PFN_vkCreateImage)dev_proc(device, "vkCreateImage");
    sc->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)dev_proc(device, "vkGetImageMemoryRequirements");
    sc->GetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout)dev_proc(device, "vkGetImageSubresourceLayout");
    sc->AllocateMemory = (PFN_vkAllocateMemory)dev_proc(device, "vkAllocateMemory");
    sc->BindImageMemory = (PFN_vkBindImageMemory)dev_proc(device, "vkBindImageMemory");
    sc->FreeMemory = (PFN_vkFreeMemory)dev_proc(device, "vkFreeMemory");
    sc->DestroyImage = (PFN_vkDestroyImage)dev_proc(device, "vkDestroyImage");
    sc->GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)dev_proc(device, "vkGetMemoryFdKHR");
    (void)sc->GetPhysDevMemProps;
    { extern int pvr_icd_mem_props(void *, void *);
      pthread_mutex_lock(&g_memprops_mu);
      if (surf && !surf->have_mem) {
          if (pvr_icd_mem_props(surf->pd, &surf->memProps) == 0 && surf->memProps.memoryTypeCount > 0) {
              surf->have_mem = 1;
              g_cached_memProps = surf->memProps;
              g_have_cached_mem = 1;
          }
      }
      if (surf && surf->have_mem) {
          sc->memProps = surf->memProps;
      } else if (g_have_cached_mem) {
          sc->memProps = g_cached_memProps;
      } else {
          pvr_icd_mem_props(NULL, &sc->memProps);
      }
      pthread_mutex_unlock(&g_memprops_mu); }

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
        int found_exp_dev_local = 0;
        for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++) {
            if ((mr.memoryTypeBits & (1u << t)) &&
                (sc->memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mai.memoryTypeIndex = t;
                found_exp_dev_local = 1;
                break;
            }
        }
        if (!found_exp_dev_local) {
            for (uint32_t t = 0; t < 32; t++) if (mr.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
        }
        r = sc->AllocateMemory(device, &mai, alloc, &sc->images[i].mem);
        if (r != VK_SUCCESS) { WSI_LOG("vkAllocateMemory failed %d", r); goto fail; }
        r = sc->BindImageMemory(device, sc->images[i].image, sc->images[i].mem, 0);
        if (r != VK_SUCCESS) goto fail;
        /* The render target the app is handed: TILING_OPTIMAL, no external handle, same format and
         * extent, with the transfer source bit the present-time copy needs. */
        {
            VkImageCreateInfo rici = ici;
            rici.pNext = NULL;
            rici.tiling = VK_IMAGE_TILING_OPTIMAL;
            rici.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            rici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            r = sc->CreateImage(device, &rici, alloc, &sc->images[i].render);
            if (r != VK_SUCCESS) { WSI_LOG("render image create failed %d", r); goto fail; }
            VkMemoryRequirements rmr; sc->GetImageMemoryRequirements(device, sc->images[i].render, &rmr);
            VkMemoryDedicatedAllocateInfo rded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
            rded.image = sc->images[i].render;
            VkMemoryAllocateInfo rmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            rmai.pNext = &rded; rmai.allocationSize = rmr.size; rmai.memoryTypeIndex = 0;
            int found_dev_local = 0;
            for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++) {
                if ((rmr.memoryTypeBits & (1u << t)) &&
                    (sc->memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    rmai.memoryTypeIndex = t;
                    found_dev_local = 1;
                    break;
                }
            }
            if (!found_dev_local) {
                for (uint32_t t = 0; t < 32; t++) if (rmr.memoryTypeBits & (1u << t)) { rmai.memoryTypeIndex = t; break; }
            }
            r = sc->AllocateMemory(device, &rmai, alloc, &sc->images[i].render_mem);
            if (r != VK_SUCCESS) { WSI_LOG("render image alloc failed %d", r); goto fail; }
            r = sc->BindImageMemory(device, sc->images[i].render, sc->images[i].render_mem, 0);
            if (r != VK_SUCCESS) { WSI_LOG("render image bind failed %d", r); goto fail; }
        }
        /* The real pitch, not width*4: a linear image may be padded, and the wl_buffer must be told
         * the pitch the driver actually used or the compositor reads the wrong rows. */
        VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
        VkSubresourceLayout layout; memset(&layout, 0, sizeof(layout));
        if (sc->GetImageSubresourceLayout) sc->GetImageSubresourceLayout(device, sc->images[i].image, &subres, &layout);
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
         * (see wsi_clear_image_once), and clear sc->images[i].render too so both start in GENERAL. */
        pthread_mutex_lock(&wsi_lock);
        wsi_clear_image_once(device, sc->queue, sc->images[i].image, ci->imageFormat,
                             (uint32_t)sc->width, (uint32_t)sc->height);
        if (sc->images[i].render != VK_NULL_HANDLE) {
            wsi_clear_image_once(device, sc->queue, sc->images[i].render, ci->imageFormat,
                                 (uint32_t)sc->width, (uint32_t)sc->height);
        }
        pthread_mutex_unlock(&wsi_lock);
        made = i + 1;
    }
    WSI_LOG("swapchain: %u images %dx%d stride %u fmt %d (fd0=%d)", sc->count, sc->width, sc->height,
            sc->stride, ci->imageFormat, sc->images[0].fd);
    *out = (VkSwapchainKHR)sc;
    return VK_SUCCESS;
fail:
    /* Destroy what was made, so a failure to build the swapchain does not leak images, memory and fds. */
    (void)made;
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->images[i].fd >= 0) { close(sc->images[i].fd); sc->images[i].fd = -1; }
        if (sc->images[i].render && sc->DestroyImage) sc->DestroyImage(device, sc->images[i].render, alloc);
        if (sc->images[i].render_mem && sc->FreeMemory) sc->FreeMemory(device, sc->images[i].render_mem, alloc);
        if (sc->images[i].image && sc->DestroyImage) sc->DestroyImage(device, sc->images[i].image, alloc);
        if (sc->images[i].mem && sc->FreeMemory) sc->FreeMemory(device, sc->images[i].mem, alloc);
    }
    if (sc->dmabuf_wrap) { wl_proxy_wrapper_destroy(sc->dmabuf_wrap); sc->dmabuf_wrap = NULL; }
    if (sc->wl_queue) { wl_event_queue_destroy(sc->wl_queue); sc->wl_queue = NULL; }
    pthread_mutex_destroy(&sc->lock);
    free(sc);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static void wsi_destroy_swapchain(VkDevice device, VkSwapchainKHR sw, const VkAllocationCallbacks *alloc) {
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    if (!sc) return;
    WSI_LOG("destroy_swapchain: sw=%p %dx%d", (void *)sw, sc->width, sc->height);
    pthread_mutex_lock(&wsi_lock);
    struct wsi_device_entries ecopy;
    pthread_mutex_t *qmu = NULL;
    int have_dev = wsi_copy_for_dev(device, &ecopy, &qmu);
    if (have_dev && ecopy.QueueWaitIdle && sc->queue) {
        pthread_mutex_lock(qmu);
        ecopy.QueueWaitIdle(sc->queue);
        pthread_mutex_unlock(qmu);
    }
    if (sc->blit_fence && have_dev && ecopy.DestroyFence) {
        ecopy.DestroyFence(device, sc->blit_fence, alloc);
        sc->blit_fence = VK_NULL_HANDLE;
    }
    if (sc->blit_pool) {
        PFN_vkDestroyCommandPool pDestroyPool = (PFN_vkDestroyCommandPool)dev_proc(device, "vkDestroyCommandPool");
        if (pDestroyPool) pDestroyPool(device, sc->blit_pool, alloc);
        sc->blit_pool = VK_NULL_HANDLE;
        sc->blit_cb = VK_NULL_HANDLE;
    }
    /* The compositor may still hold a wl_buffer on a presented image; drop our reference without
     * waiting, then release everything we own. Destroy wl_buffers BEFORE wl_queue/wrapper. */
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->images[i].buffer) { wl_buffer_destroy(sc->images[i].buffer); sc->images[i].buffer = NULL; }
        if (sc->images[i].pixmap && sc->surface && sc->surface->kind == WSI_X11 &&
            sc->surface->c && !xcb_connection_has_error(sc->surface->c)) {
            xcb_free_pixmap(sc->surface->c, sc->images[i].pixmap);
            xcb_flush(sc->surface->c);
        }
        if (sc->images[i].fd >= 0) { close(sc->images[i].fd); sc->images[i].fd = -1; }
        if (sc->images[i].render && sc->DestroyImage) sc->DestroyImage(device, sc->images[i].render, alloc);
        if (sc->images[i].render_mem && sc->FreeMemory) sc->FreeMemory(device, sc->images[i].render_mem, alloc);
        if (sc->images[i].image && sc->DestroyImage) sc->DestroyImage(device, sc->images[i].image, alloc);
        if (sc->images[i].mem && sc->FreeMemory) sc->FreeMemory(device, sc->images[i].mem, alloc);
    }
    if (sc->dmabuf_wrap) { wl_proxy_wrapper_destroy(sc->dmabuf_wrap); sc->dmabuf_wrap = NULL; }
    if (sc->wl_queue) { wl_event_queue_destroy(sc->wl_queue); sc->wl_queue = NULL; }
    pthread_mutex_unlock(&wsi_lock);
    pthread_mutex_destroy(&sc->lock);
    free(sc);
}

static VkResult wsi_get_swapchain_images(VkDevice device, VkSwapchainKHR sw, uint32_t *count, VkImage *images) {
    (void)device;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    WSI_LOG("get_images: sw=%p want=%u have=%u -> %s", (void *)sw, images ? *count : 0, sc->count, images ? "fill" : "count");
    if (!images) { *count = sc->count; return VK_SUCCESS; }
    uint32_t n = *count < sc->count ? *count : sc->count;
    for (uint32_t i = 0; i < n; i++) images[i] = sc->images[i].render ? sc->images[i].render : sc->images[i].image;
    *count = n;
    return VK_SUCCESS;
}

static uint32_t wsi_pick_free_image(struct wsi_swapchain *sc, int require_unbusy) {
    for (uint32_t k = 0; k < sc->count; k++) {
        uint32_t j = (sc->pending + k) % sc->count;
        if (!sc->images[j].acquired && (!require_unbusy || !sc->images[j].busy)) return j;
    }
    return UINT32_MAX;
}

static VkResult wsi_acquire_next_image(VkDevice device, VkSwapchainKHR sw, uint64_t timeout,
                                       VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)timeout;
    static int acq_log_n = 0;
    int log_it = (acq_log_n < 16 || wsi_verbose_log());
    if (log_it) {
        acq_log_n++;
        WSI_LOG("acquire: enter sw=%p sem=%p fence=%p", (void *)sw, (void *)sem, (void *)fence);
    }
    if (!sw || !index) return VK_ERROR_INITIALIZATION_FAILED;
    struct wsi_swapchain *sc = (struct wsi_swapchain *)sw;
    pthread_mutex_lock(&sc->lock);
    if (sc->wl_queue) wsi_drain_wl_queue(sc, 0);
    uint32_t idx = wsi_pick_free_image(sc, 1);
    int wait_ms = (timeout == 0) ? 0 : (timeout >= 250000000ull ? 250 : (int)((timeout + 999999ull) / 1000000ull));
    if (timeout > 0 && wait_ms < 1) wait_ms = 1;
    for (int waited = 0; idx == UINT32_MAX && sc->wl_queue && waited < wait_ms; ) {
        int slice = wait_ms - waited;
        if (slice > 25) slice = 25;
        wsi_drain_wl_queue(sc, slice);
        waited += slice;
        idx = wsi_pick_free_image(sc, 1);
    }
    if (idx == UINT32_MAX && timeout == 0) {
        pthread_mutex_unlock(&sc->lock);
        return VK_NOT_READY;
    }
    if (idx == UINT32_MAX) {
        static int busy_acq_warn = 0;
        if (busy_acq_warn < 8) {
            busy_acq_warn++;
            WSI_LOG("acquire: sw=%p no unbusy image after %dms, falling back", (void *)sw, wait_ms);
        }
        idx = wsi_pick_free_image(sc, 0);
    }
    if (idx == UINT32_MAX) idx = sc->pending;
    sc->images[idx].acquired = 1;
    sc->pending = (idx + 1) % sc->count;
    int ok = wsi_signal_on_queue(device, sc->queue, fence, sem);
    if (!ok && (sem || fence)) sc->images[idx].acquired = 0;
    pthread_mutex_unlock(&sc->lock);
    if (!ok && (sem || fence)) {
        /* Say so rather than hand back an image the app will wait on forever. */
        WSI_LOG("acquire: no HAL vkQueueSubmit for %p; the frame would hang", (void *)device);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *index = idx;
    if (log_it) WSI_LOG("acquire: sw=%p -> image %u", (void *)sw, idx);
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

/* Wait for binary semaphores the app signaled through vkQueueSubmit. There is no host-side
 * vkWaitSemaphores for binary semaphores, so the wait is expressed as a single queue operation: an
 * empty submit taking all wait semaphores and a fresh fence, then waiting on that fence outside
 * queue_mu so other threads (like Zink's zcfq0) can keep submitting without deadlocking. */
static VkResult wsi_wait_semaphores(VkDevice dev, VkQueue queue, uint32_t count, const VkSemaphore *sems) {
    if (!count || !sems) return VK_SUCCESS;
    struct wsi_device_entries ecopy;
    pthread_mutex_t *qmu = NULL;
    if (!wsi_copy_for_dev(dev, &ecopy, &qmu) || !ecopy.QueueSubmit || !ecopy.CreateFence ||
        !ecopy.WaitForFences || !ecopy.DestroyFence) {
        WSI_LOG("present: no HAL fence entries for %p; not waiting on the semaphore", (void *)dev);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkSemaphore local_sems[16];
    VkPipelineStageFlags local_stages[16];
    VkSemaphore *wsems = count <= 16 ? local_sems : malloc(sizeof(VkSemaphore) * count);
    VkPipelineStageFlags *wstages = count <= 16 ? local_stages : malloc(sizeof(VkPipelineStageFlags) * count);
    if (!wsems || !wstages) {
        if (wsems != local_sems) free(wsems);
        if (wstages != local_stages) free(wstages);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (sems[i] != VK_NULL_HANDLE) {
            wsems[n] = sems[i];
            wstages[n] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            n++;
        }
    }
    VkResult r = VK_SUCCESS;
    if (n > 0) {
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence f = VK_NULL_HANDLE;
        r = ecopy.CreateFence(dev, &fci, NULL, &f);
        if (r == VK_SUCCESS) {
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = n;
            si.pWaitSemaphores = wsems;
            si.pWaitDstStageMask = wstages;
            pthread_mutex_lock(qmu);
            r = ecopy.QueueSubmit(queue, 1, &si, f);
            pthread_mutex_unlock(qmu);
            if (r == VK_SUCCESS) r = ecopy.WaitForFences(dev, 1, &f, VK_TRUE, 1000000000ull /* 1s */);
            ecopy.DestroyFence(dev, f, NULL);
        }
    }
    if (wsems != local_sems) free(wsems);
    if (wstages != local_stages) free(wstages);
    return r;
}

/* What the compositor will actually get. Reads the very dma-buf about to be handed to Xwayland/Wayland
 * and reports its mean, so "black on screen" can be split into its two possible causes with one line:
 * black here means the client's own raster (or the buffer we export is not the one it rendered into);
 * non-black here means the hand-off - a missing finish/fence, or a different fd/pitch reaching the
 * compositor. Four rows sampled with pread: no mapping, no format assumptions, safe on tiled memory
 * because a tiled image would already be an import bug this dump is meant to expose. */
static void probe_export(const struct wsi_swapchain *sc, uint32_t idx, const char *where) {
    static int logged;
    if (!sc || idx >= sc->count || sc->images[idx].fd < 0) return;
    if (logged >= 60) return;
    logged++;
    const int fd = sc->images[idx].fd;
    const uint32_t stride = sc->stride;
    const size_t bytes = (size_t)stride * (size_t)sc->height;
    /* mmap, not pread: a dma-buf fd is not seekable, so pread() on it returns -1 and the probe would
     * report a mean of -1 (measured: export[x11] ... mean=-1.0 nonblack_bytes=0/0). */
    if (bytes == 0 || sc->width <= 0 || sc->height <= 0) return;
    unsigned char *map = (unsigned char *)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) /* a dma-buf exports its mmap with write access, not read-only */
        map = (unsigned char *)mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { WSI_LOG("export[%s] mmap failed: %s", where, strerror(errno)); return; }
    double sum = 0; long n = 0; int nonblack = 0;
    for (int r = 0; r < 4; r++) {
        const unsigned char *rowp = map + (size_t)stride * (size_t)((sc->height * r) / 4);
        const uint32_t row = stride < 256 ? stride : 256;
        for (uint32_t i = 0; i < row; i++) { sum += rowp[i]; if (rowp[i]) nonblack++; }
    }
    n = (long)((stride < 256 ? stride : 256) * 4);
    munmap(map, bytes);
    WSI_LOG("export[%s] image=%u fd=%d %dx%d stride=%u rows4 mean=%.1f nonblack_bytes=%ld/%ld",
            where, idx, fd, sc->width, sc->height, stride, n ? sum / (double)n : -1.0, (long)nonblack, n);
}

/* The fd route is closed (a dma-buf is not seekable and its mmap is refused EACCES), so the content
 * test goes through the API: copy the very image being presented into a host-visible buffer, wait,
 * and report what is in it. One run per swapchain image, four sampled rows plus four quadrants, so
 * the answer is "does the frame the compositor is about to receive contain anything". */
static void probe_vk_readback(struct wsi_swapchain *sc, uint32_t idx, const char *where) {
    static int logged;
    if (!sc || idx >= sc->count) return;
    /* Only the picture: Steam's X11 helper popups are 32x32/64x24 and are never mapped (map=0), and
     * they were consuming the whole probe budget before the real 1602x720 surface appeared. */
    if (sc->width < 800 || sc->height < 400) return;
    if (logged >= 40) return;
    logged++;
    VkDevice dev = sc->device;
    PFN_vkCreateBuffer pCreateBuffer = (PFN_vkCreateBuffer)dev_proc(dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements pGetBufReq = (PFN_vkGetBufferMemoryRequirements)dev_proc(dev, "vkGetBufferMemoryRequirements");
    PFN_vkBindBufferMemory pBindBuffer = (PFN_vkBindBufferMemory)dev_proc(dev, "vkBindBufferMemory");
    PFN_vkCreateCommandPool pCreatePool = (PFN_vkCreateCommandPool)dev_proc(dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers pAllocCmd = (PFN_vkAllocateCommandBuffers)dev_proc(dev, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer pBegin = (PFN_vkBeginCommandBuffer)dev_proc(dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer pEnd = (PFN_vkEndCommandBuffer)dev_proc(dev, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier pBarrier = (PFN_vkCmdPipelineBarrier)dev_proc(dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer pCopy = (PFN_vkCmdCopyImageToBuffer)dev_proc(dev, "vkCmdCopyImageToBuffer");
    PFN_vkMapMemory pMap = (PFN_vkMapMemory)dev_proc(dev, "vkMapMemory");
    PFN_vkUnmapMemory pUnmap = (PFN_vkUnmapMemory)dev_proc(dev, "vkUnmapMemory");
    PFN_vkDestroyBuffer pDestroyBuffer = (PFN_vkDestroyBuffer)dev_proc(dev, "vkDestroyBuffer");
    PFN_vkFreeCommandBuffers pFreeCmd = (PFN_vkFreeCommandBuffers)dev_proc(dev, "vkFreeCommandBuffers");
    PFN_vkDestroyCommandPool pDestroyPool = (PFN_vkDestroyCommandPool)dev_proc(dev, "vkDestroyCommandPool");
    struct wsi_device_entries *e = wsi_entries(dev);
    if (!pCreateBuffer || !pGetBufReq || !pBindBuffer || !pCreatePool || !pAllocCmd || !pBegin ||
        !pEnd || !pBarrier || !pCopy || !pMap || !pUnmap || !pDestroyBuffer || !pFreeCmd ||
        !pDestroyPool || !e || !e->QueueSubmit || !e->CreateFence || !e->WaitForFences || !e->DestroyFence) {
        WSI_LOG("readback[%s]: HAL entrypoints unavailable", where);
        return;
    }
    const VkDeviceSize size = (VkDeviceSize)sc->stride * (VkDeviceSize)sc->height;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf = VK_NULL_HANDLE;
    if (pCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) { WSI_LOG("readback[%s]: create buffer failed", where); return; }
    VkMemoryRequirements req; pGetBufReq(dev, buf, &req);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (!sc->memProps.memoryTypeCount && sc->surface) sc->memProps = sc->surface->memProps;
    /* Host-visible is the only requirement (coherency just saves a flush); requiring both matched no
     * type on this HAL and ended the probe at "no host-visible memory type". */
    int type = -1, coherent = -1;
    for (uint32_t t = 0; t < sc->memProps.memoryTypeCount; t++) {
        if (!(req.memoryTypeBits & (1u << t))) continue;
        const VkMemoryPropertyFlags f = sc->memProps.memoryTypes[t].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        if (type < 0) type = (int)t;
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) { coherent = (int)t; break; }
    }
    if (coherent >= 0) type = coherent;
    if (type < 0) {
        WSI_LOG("readback[%s]: no host-visible type (bits=0x%x types=%u)", where, req.memoryTypeBits,
                sc->memProps.memoryTypeCount);
        pDestroyBuffer(dev, buf, NULL);
        return;
    }
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type };
    if (sc->AllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS || pBindBuffer(dev, buf, mem, 0) != VK_SUCCESS) {
        WSI_LOG("readback[%s]: buffer memory failed", where); pDestroyBuffer(dev, buf, NULL); if (mem) sc->FreeMemory(dev, mem, NULL); return;
    }
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = e->queue_family };
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (pCreatePool(dev, &pci, NULL, &pool) != VK_SUCCESS) { WSI_LOG("readback[%s]: pool failed", where); goto out_buf; }
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (pAllocCmd(dev, &cai, &cmd) != VK_SUCCESS) { WSI_LOG("readback[%s]: cmdbuf failed", where); goto out_pool; }
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    pBegin(cmd, &bi);
    VkImage target_img = (!strcmp(where, "wl-render") || !strcmp(where, "x11-render"))
        ? sc->images[idx].render : sc->images[idx].image;
    if (!target_img) return;
    VkImageMemoryBarrier bar = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    pBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy region = { .bufferOffset = 0, .bufferRowLength = sc->stride / 4, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 }, .imageExtent = { (uint32_t)sc->width, (uint32_t)sc->height, 1 } };
    pCopy(cmd, target_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    pEnd(cmd);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    e->CreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VkResult r = e->QueueSubmit(sc->queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = e->WaitForFences(dev, 1, &fence, VK_TRUE, 500000000ull);
    WSI_LOG("readback[%s]: submit=%d fence=%d", where, r, fence != VK_NULL_HANDLE);
    if (r == VK_SUCCESS) {
        void *p = NULL;
        if (pMap(dev, mem, 0, size, 0, &p) == VK_SUCCESS && p) {
            const unsigned char *base = (const unsigned char *)p;
            double sum = 0; long n = 0, nonblack = 0;
            unsigned char qpx[4][4] = {{0}};
            for (int q = 0; q < 4; q++) {
                long y = (long)(sc->height) * (q / 2) / 2 + 4;
                long x0 = (long)(sc->width) * (q % 2) / 2;
                const unsigned char *first = base + y * (long)sc->stride + x0 * 4;
                qpx[q][0] = first[0]; qpx[q][1] = first[1]; qpx[q][2] = first[2]; qpx[q][3] = first[3];
                for (int yy = 0; yy < 8; yy++) {
                    const unsigned char *row = base + (y + yy) * (long)sc->stride + x0 * 4;
                    for (int xx = 0; xx < 64; xx++) {
                        const unsigned char *px = row + xx * 4;
                        sum += px[0] + px[1] + px[2]; n += 3;
                        if (px[0] | px[1] | px[2]) nonblack++;
                    }
                }
            }
            WSI_LOG("readback[%s] image=%u %dx%d stride=%u mean=%.2f nonblack_px=%ld/%d q0=(%u,%u,%u,%u) q1=(%u,%u,%u,%u) q2=(%u,%u,%u,%u) q3=(%u,%u,%u,%u)",
                    where, idx, sc->width, sc->height, sc->stride, n ? sum / (double)n : -1.0, nonblack, 4 * 8 * 64,
                    qpx[0][0], qpx[0][1], qpx[0][2], qpx[0][3],
                    qpx[1][0], qpx[1][1], qpx[1][2], qpx[1][3],
                    qpx[2][0], qpx[2][1], qpx[2][2], qpx[2][3],
                    qpx[3][0], qpx[3][1], qpx[3][2], qpx[3][3]);
            pUnmap(dev, mem);
        } else WSI_LOG("readback[%s]: map failed", where);
    }
    if (fence) e->DestroyFence(dev, fence, NULL);
    pFreeCmd(dev, pool, 1, &cmd);
out_pool:
    pDestroyPool(dev, pool, NULL);
out_buf:
    pDestroyBuffer(dev, buf, NULL);
    sc->FreeMemory(dev, mem, NULL);
}

/* Hand the app an image, then copy it into the exportable one at present.
 *
 * The app is given the TILING_OPTIMAL image (sc->images[idx].render); the LINEAR image whose dma-buf
 * gamescope imports never sees a render pass from the app. Both transitions are explicit, the copy is
 * the whole extent at the format the ICD exposes, and the command resources are made once per
 * swapchain so a present costs one submit rather than a pool + buffer + fence allocation (churn on
 * every frame is the kind of stutter that reads as flashing).
 */
/* The same Vulkan readback, for an image the WSI did not create: CEF's own dynamic-rendering colour
 * attachment, which is the copy source at present time. Its layout is taken as GENERAL (what CEF
 * leaves it in); the copy reports the pixels either way, and the caller logs the image's create info
 * so the tiling/format that produced a black frame is on the record. */
static void probe_any_image(struct wsi_swapchain *sc, VkImage img, int iw, int ih, const char *tag) {
    if (!sc || img == VK_NULL_HANDLE || iw <= 0 || ih <= 0) return;
    static int n; if (n++ > 6) return;
    VkDevice dev = sc->device;
    struct wsi_device_entries *e = wsi_entries(dev);
    PFN_vkCreateBuffer pCreateBuffer = (PFN_vkCreateBuffer)dev_proc(dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements pGetBufReq = (PFN_vkGetBufferMemoryRequirements)dev_proc(dev, "vkGetBufferMemoryRequirements");
    PFN_vkBindBufferMemory pBindBuffer = (PFN_vkBindBufferMemory)dev_proc(dev, "vkBindBufferMemory");
    PFN_vkCreateCommandPool pCreatePool = (PFN_vkCreateCommandPool)dev_proc(dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers pAllocCmd = (PFN_vkAllocateCommandBuffers)dev_proc(dev, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer pBegin = (PFN_vkBeginCommandBuffer)dev_proc(dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer pEnd = (PFN_vkEndCommandBuffer)dev_proc(dev, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier pBarrier = (PFN_vkCmdPipelineBarrier)dev_proc(dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer pCopy = (PFN_vkCmdCopyImageToBuffer)dev_proc(dev, "vkCmdCopyImageToBuffer");
    PFN_vkMapMemory pMap = (PFN_vkMapMemory)dev_proc(dev, "vkMapMemory");
    PFN_vkDestroyBuffer pDestroyBuffer = (PFN_vkDestroyBuffer)dev_proc(dev, "vkDestroyBuffer");
    PFN_vkFreeCommandBuffers pFreeCmd = (PFN_vkFreeCommandBuffers)dev_proc(dev, "vkFreeCommandBuffers");
    PFN_vkDestroyCommandPool pDestroyPool = (PFN_vkDestroyCommandPool)dev_proc(dev, "vkDestroyCommandPool");
    if (!e || !pCreateBuffer || !pGetBufReq || !pBindBuffer || !pCreatePool || !pAllocCmd || !pBegin ||
        !pEnd || !pBarrier || !pCopy || !pMap || !pDestroyBuffer || !pFreeCmd || !pDestroyPool) return;
    const VkDeviceSize stride = (VkDeviceSize)iw * 4;
    const VkDeviceSize size = stride * (VkDeviceSize)ih;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (pCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) return;
    VkMemoryRequirements req; pGetBufReq(dev, buf, &req);
    int type = -1;
    for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (sc->memProps.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) { type = (int)t; break; }
    }
    if (type < 0) {
        for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++)
            if ((req.memoryTypeBits & (1u << t)) &&
                (sc->memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = (int)t; break; }
    }
    if (type < 0) { pDestroyBuffer(dev, buf, NULL); return; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (sc->AllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS ||
        pBindBuffer(dev, buf, mem, 0) != VK_SUCCESS) {
        pDestroyBuffer(dev, buf, NULL);
        if (mem) sc->FreeMemory(dev, mem, NULL);
        return;
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = e->queue_family;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cb = VK_NULL_HANDLE;
    if (pCreatePool(dev, &pci, NULL, &pool) != VK_SUCCESS) { pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL); return; }
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (pAllocCmd(dev, &cai, &cb) != VK_SUCCESS) { pDestroyPool(dev, pool, NULL); pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL); return; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBegin(cb, &bi);
    VkImageMemoryBarrier bar; memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img; bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1; bar.subresourceRange.layerCount = 1;
    pBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy region; memset(&region, 0, sizeof region);
    region.bufferRowLength = (uint32_t)iw; region.bufferImageHeight = (uint32_t)ih;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)iw; region.imageExtent.height = (uint32_t)ih; region.imageExtent.depth = 1;
    pCopy(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    pEnd(cb);
    pthread_mutex_t *qmu = NULL;
    struct wsi_device_entries ecopy;
    if (!wsi_copy_for_dev(dev, &ecopy, &qmu)) qmu = NULL;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE; e->CreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (qmu) pthread_mutex_lock(qmu);
    VkResult r = e->QueueSubmit(sc->queue, 1, &si, fence);
    if (qmu) pthread_mutex_unlock(qmu);
    if (r == VK_SUCCESS) r = e->WaitForFences(dev, 1, &fence, VK_TRUE, 500000000ull);
    if (r == VK_SUCCESS) {
        void *pp = NULL;
        if (pMap(dev, mem, 0, size, 0, &pp) == VK_SUCCESS && pp) {
            const unsigned char *base = (const unsigned char *)pp;
            double sum = 0; long n2 = 0, nonblack = 0;
            for (int q = 0; q < 4; q++) {
                long y = (long)ih * (q / 2) / 2 + 4, x0 = (long)iw * (q % 2) / 2;
                for (int yy = 0; yy < 8; yy++) {
                    const unsigned char *row = base + (y + yy) * (long)stride + x0 * 4;
                    for (int xx = 0; xx < 64 && (x0 + xx) < (long)iw; xx++) {
                        const unsigned char *px = row + xx * 4;
                        sum += px[0] + px[1] + px[2]; n2 += 3;
                        if (px[0] | px[1] | px[2]) nonblack++;
                    }
                }
            }
            WSI_LOG("readback[%s] %dx%d mean=%.2f nonblack_px=%ld/2048", tag, iw, ih,
                    n2 ? sum / (double)n2 : -1.0, nonblack);
        }
    } else WSI_LOG("readback[%s]: submit=%d (source not readable)", tag, r);
    if (fence) e->DestroyFence(dev, fence, NULL);
    pFreeCmd(dev, pool, 1, &cb); pDestroyPool(dev, pool, NULL);
    pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL);
}

static int wsi_blit_to_export(struct wsi_swapchain *sc, uint32_t idx, VkQueue q,
                              uint32_t wait_count, const VkSemaphore *wait_sems) {
    if (!sc || idx >= sc->count || sc->images[idx].render == VK_NULL_HANDLE) return 0;
    VkQueue submit_q = q ? q : sc->queue;
    VkDevice dev = sc->device;
    struct wsi_device_entries ecopy;
    pthread_mutex_t *qmu = NULL;
    if (!wsi_copy_for_dev(dev, &ecopy, &qmu) || !ecopy.QueueSubmit || !ecopy.CreateFence || !ecopy.WaitForFences)
        return 0;
    if (!sc->blit_Begin) {
        sc->blit_Begin = (PFN_vkBeginCommandBuffer)dev_proc(dev, "vkBeginCommandBuffer");
        sc->blit_End = (PFN_vkEndCommandBuffer)dev_proc(dev, "vkEndCommandBuffer");
        sc->blit_Barrier = (PFN_vkCmdPipelineBarrier)dev_proc(dev, "vkCmdPipelineBarrier");
        sc->blit_Copy = (PFN_vkCmdCopyImage)dev_proc(dev, "vkCmdCopyImage");
        sc->blit_Reset = (PFN_vkResetCommandBuffer)dev_proc(dev, "vkResetCommandBuffer");
        sc->blit_ResetFences = (PFN_vkResetFences)dev_proc(dev, "vkResetFences");
    }
    PFN_vkBeginCommandBuffer pBegin = sc->blit_Begin;
    PFN_vkEndCommandBuffer pEnd = sc->blit_End;
    PFN_vkCmdPipelineBarrier pBarrier = sc->blit_Barrier;
    PFN_vkCmdCopyImage pCopy = sc->blit_Copy;
    PFN_vkResetCommandBuffer pReset = sc->blit_Reset;
    PFN_vkResetFences pResetFences = sc->blit_ResetFences;
    if (!pBegin || !pEnd || !pBarrier || !pCopy) return 0;
    if (!sc->blit_pool) {
        PFN_vkCreateCommandPool pCreatePool = (PFN_vkCreateCommandPool)dev_proc(dev, "vkCreateCommandPool");
        PFN_vkAllocateCommandBuffers pAlloc = (PFN_vkAllocateCommandBuffers)dev_proc(dev, "vkAllocateCommandBuffers");
        if (!pCreatePool || !pAlloc) return 0;
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.queueFamilyIndex = ecopy.queue_family;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (pCreatePool(dev, &pci, NULL, &sc->blit_pool) != VK_SUCCESS) { sc->blit_pool = VK_NULL_HANDLE; return 0; }
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = sc->blit_pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (pAlloc(dev, &cai, &sc->blit_cb) != VK_SUCCESS) return 0;
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (ecopy.CreateFence(dev, &fci, NULL, &sc->blit_fence) != VK_SUCCESS) return 0;
    }
    if (pReset) pReset(sc->blit_cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (pBegin(sc->blit_cb, &bi) != VK_SUCCESS) return 0;
    VkImageMemoryBarrier bars[2];
    memset(bars, 0, sizeof(bars));
    bars[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bars[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bars[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].image = sc->images[idx].render;
    bars[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bars[0].subresourceRange.levelCount = 1;
    bars[0].subresourceRange.layerCount = 1;
    bars[1] = bars[0];
    bars[1].srcAccessMask = 0;
    bars[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bars[1].image = sc->images[idx].image;
    pBarrier(sc->blit_cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
             0, NULL, 0, NULL, 2, bars);
    VkImageCopy region;
    memset(&region, 0, sizeof(region));
    region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = 1;
    region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.dstSubresource.layerCount = 1;
    region.extent.width = (uint32_t)sc->width;
    region.extent.height = (uint32_t)sc->height;
    region.extent.depth = 1;
    pCopy(sc->blit_cb, sc->images[idx].render, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          sc->images[idx].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    /* Restore BOTH render and exportable images to GENERAL so render is never left stuck in TRANSFER_SRC_OPTIMAL. */
    VkImageMemoryBarrier back[2];
    back[0] = bars[0];
    back[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    back[1] = bars[1];
    back[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    back[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    back[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    back[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pBarrier(sc->blit_cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
             0, NULL, 0, NULL, 2, back);
    if (pEnd(sc->blit_cb) != VK_SUCCESS) return 0;
    if (pResetFences) pResetFences(dev, 1, &sc->blit_fence);
    VkSemaphore local_sems[16];
    VkPipelineStageFlags local_stages[16];
    uint32_t valid_waits = 0;
    if (wait_count > 0 && wait_sems) {
        uint32_t cap = wait_count < 16 ? wait_count : 16;
        for (uint32_t k = 0; k < cap; k++) {
            if (wait_sems[k] != VK_NULL_HANDLE) {
                local_sems[valid_waits] = wait_sems[k];
                local_stages[valid_waits] = VK_PIPELINE_STAGE_TRANSFER_BIT;
                valid_waits++;
            }
        }
    }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = valid_waits;
    si.pWaitSemaphores = valid_waits ? local_sems : NULL;
    si.pWaitDstStageMask = valid_waits ? local_stages : NULL;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc->blit_cb;
    pthread_mutex_lock(qmu);
    VkResult r = ecopy.QueueSubmit(submit_q, 1, &si, sc->blit_fence);
    pthread_mutex_unlock(qmu);
    if (r == VK_SUCCESS) r = ecopy.WaitForFences(dev, 1, &sc->blit_fence, VK_TRUE, 1000000000ull);
    { static int n, last_w, last_h;
      if (n < 32 || sc->width != last_w || sc->height != last_h || r != VK_SUCCESS) {
          n++; last_w = sc->width; last_h = sc->height;
          WSI_LOG("blit[%u]: %dx%d optimal->linear submit=%d", idx, sc->width, sc->height, r);
      } }
    return r == VK_SUCCESS;
}

static int wsi_x11_put_image_present(struct wsi_swapchain *sc, uint32_t idx) {
    struct wsi_surface *surf = sc->surface;
    if (!surf || !surf->c || xcb_connection_has_error(surf->c)) return 0;
    VkDevice dev = sc->device;
    struct wsi_device_entries *e = wsi_entries(dev);
    PFN_vkCreateBuffer pCreateBuffer = (PFN_vkCreateBuffer)dev_proc(dev, "vkCreateBuffer");
    PFN_vkGetBufferMemoryRequirements pGetBufReq = (PFN_vkGetBufferMemoryRequirements)dev_proc(dev, "vkGetBufferMemoryRequirements");
    PFN_vkBindBufferMemory pBindBuffer = (PFN_vkBindBufferMemory)dev_proc(dev, "vkBindBufferMemory");
    PFN_vkCreateCommandPool pCreatePool = (PFN_vkCreateCommandPool)dev_proc(dev, "vkCreateCommandPool");
    PFN_vkAllocateCommandBuffers pAllocCmd = (PFN_vkAllocateCommandBuffers)dev_proc(dev, "vkAllocateCommandBuffers");
    PFN_vkBeginCommandBuffer pBegin = (PFN_vkBeginCommandBuffer)dev_proc(dev, "vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer pEnd = (PFN_vkEndCommandBuffer)dev_proc(dev, "vkEndCommandBuffer");
    PFN_vkCmdPipelineBarrier pBarrier = (PFN_vkCmdPipelineBarrier)dev_proc(dev, "vkCmdPipelineBarrier");
    PFN_vkCmdCopyImageToBuffer pCopy = (PFN_vkCmdCopyImageToBuffer)dev_proc(dev, "vkCmdCopyImageToBuffer");
    PFN_vkMapMemory pMap = (PFN_vkMapMemory)dev_proc(dev, "vkMapMemory");
    PFN_vkUnmapMemory pUnmap = (PFN_vkUnmapMemory)dev_proc(dev, "vkUnmapMemory");
    PFN_vkDestroyBuffer pDestroyBuffer = (PFN_vkDestroyBuffer)dev_proc(dev, "vkDestroyBuffer");
    PFN_vkFreeCommandBuffers pFreeCmd = (PFN_vkFreeCommandBuffers)dev_proc(dev, "vkFreeCommandBuffers");
    PFN_vkDestroyCommandPool pDestroyPool = (PFN_vkDestroyCommandPool)dev_proc(dev, "vkDestroyCommandPool");
    if (!e || !e->QueueSubmit || !e->CreateFence || !e->WaitForFences || !pCreateBuffer || !pGetBufReq ||
        !pBindBuffer || !pCreatePool || !pAllocCmd || !pBegin || !pEnd || !pBarrier || !pCopy || !pMap ||
        !pDestroyBuffer || !pFreeCmd || !pDestroyPool) return 0;

    int iw = sc->width, ih = sc->height;
    if (iw <= 0 || ih <= 0) return 0;
    VkImage src_img = sc->images[idx].render ? sc->images[idx].render : sc->images[idx].image;
    if (src_img == VK_NULL_HANDLE) return 0;

    if (!sc->memProps.memoryTypeCount) {
        extern int pvr_icd_mem_props(void *, void *);
        pthread_mutex_lock(&g_memprops_mu);
        if (surf->have_mem) sc->memProps = surf->memProps;
        else if (g_have_cached_mem) sc->memProps = g_cached_memProps;
        else pvr_icd_mem_props(surf->pd, &sc->memProps);
        pthread_mutex_unlock(&g_memprops_mu);
    }

    const VkDeviceSize row_bytes = (VkDeviceSize)iw * 4;
    const VkDeviceSize size = row_bytes * (VkDeviceSize)ih;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (pCreateBuffer(dev, &bci, NULL, &buf) != VK_SUCCESS) {
        WSI_LOG("wsi_x11_put_image_present: vkCreateBuffer failed (size=%llu)", (unsigned long long)size);
        return 0;
    }
    VkMemoryRequirements req; pGetBufReq(dev, buf, &req);
    int type = -1;
    for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++) {
        if ((req.memoryTypeBits & (1u << t)) &&
            (sc->memProps.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) { type = (int)t; break; }
    }
    if (type < 0) {
        for (uint32_t t = 0; t < sc->memProps.memoryTypeCount && t < 32; t++)
            if ((req.memoryTypeBits & (1u << t)) &&
                (sc->memProps.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { type = (int)t; break; }
    }
    if (type < 0) {
        WSI_LOG("wsi_x11_put_image_present: no host-visible memory type (bits=0x%x types=%u)",
                req.memoryTypeBits, sc->memProps.memoryTypeCount);
        pDestroyBuffer(dev, buf, NULL);
        return 0;
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = (uint32_t)type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (sc->AllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS || pBindBuffer(dev, buf, mem, 0) != VK_SUCCESS) {
        pDestroyBuffer(dev, buf, NULL);
        if (mem) sc->FreeMemory(dev, mem, NULL);
        return 0;
    }

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = e->queue_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (pCreatePool(dev, &pci, NULL, &pool) != VK_SUCCESS) { pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL); return 0; }
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (pAllocCmd(dev, &cai, &cb) != VK_SUCCESS) { pDestroyPool(dev, pool, NULL); pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL); return 0; }

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pBegin(cb, &bi);
    VkImageMemoryBarrier bar; memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL; bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = src_img; bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1; bar.subresourceRange.layerCount = 1;
    pBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &bar);
    VkBufferImageCopy region; memset(&region, 0, sizeof region);
    region.bufferRowLength = (uint32_t)iw; region.bufferImageHeight = (uint32_t)ih;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)iw; region.imageExtent.height = (uint32_t)ih; region.imageExtent.depth = 1;
    pCopy(cb, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkImageMemoryBarrier back = bar;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    pEnd(cb);

    pthread_mutex_t *qmu = NULL;
    struct wsi_device_entries ecopy;
    if (!wsi_copy_for_dev(dev, &ecopy, &qmu)) qmu = NULL;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE; e->CreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (qmu) pthread_mutex_lock(qmu);
    VkResult r = e->QueueSubmit(sc->queue, 1, &si, fence);
    if (qmu) pthread_mutex_unlock(qmu);
    if (r == VK_SUCCESS) r = e->WaitForFences(dev, 1, &fence, VK_TRUE, 500000000ull);
    int ok = 0;
    if (r == VK_SUCCESS) {
        void *pp = NULL;
        if (pMap(dev, mem, 0, size, 0, &pp) == VK_SUCCESS && pp) {
            if (!surf->x_depth) surf->x_depth = 24;
            if (!surf->gc) {
                surf->gc = xcb_generate_id(surf->c);
                xcb_create_gc(surf->c, surf->gc, surf->window, 0, NULL);
            }
            if (!sc->images[idx].pixmap) {
                sc->images[idx].pixmap = xcb_generate_id(surf->c);
                xcb_create_pixmap(surf->c, surf->x_depth, sc->images[idx].pixmap, surf->window,
                                  (uint16_t)iw, (uint16_t)ih);
            }
            const int swap_rb = (sc->drm_format == DRM_FORMAT_ABGR8888);
            int chunk_rows = (int)(65536u / (uint32_t)(iw * 4));
            if (chunk_rows < 1) chunk_rows = 1;
            if (chunk_rows > 64) chunk_rows = 64;
            uint8_t *tmp = swap_rb ? (uint8_t *)malloc((size_t)(chunk_rows * iw * 4)) : NULL;
            const uint8_t *src = (const uint8_t *)pp;
            for (int y = 0; y < ih; y += chunk_rows) {
                int rows = (y + chunk_rows <= ih) ? chunk_rows : (ih - y);
                const uint8_t *send_ptr = src + (size_t)y * (size_t)iw * 4;
                if (swap_rb && tmp) {
                    size_t n_px = (size_t)rows * (size_t)iw;
                    for (size_t p = 0; p < n_px; p++) {
                        tmp[p * 4 + 0] = send_ptr[p * 4 + 2];
                        tmp[p * 4 + 1] = send_ptr[p * 4 + 1];
                        tmp[p * 4 + 2] = send_ptr[p * 4 + 0];
                        tmp[p * 4 + 3] = send_ptr[p * 4 + 3];
                    }
                    send_ptr = tmp;
                }
                xcb_put_image(surf->c, XCB_IMAGE_FORMAT_Z_PIXMAP, sc->images[idx].pixmap, surf->gc,
                              (uint16_t)iw, (uint16_t)rows, 0, (int16_t)y, 0, surf->x_depth,
                              (uint32_t)((size_t)rows * (size_t)iw * 4), send_ptr);
            }
            if (tmp) free(tmp);
            if (surf->has_present) {
                xcb_present_pixmap(surf->c, surf->window, sc->images[idx].pixmap, ++sc->serial,
                                   XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                                   XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
            } else {
                xcb_copy_area(surf->c, sc->images[idx].pixmap, surf->window, surf->gc,
                              0, 0, 0, 0, (uint16_t)iw, (uint16_t)ih);
            }
            xcb_flush(surf->c);
            if (pUnmap) pUnmap(dev, mem);
            ok = 1;
        }
    }
    { static int put_cnt = 0; put_cnt++;
      if (put_cnt <= 5 || (put_cnt % 120) == 0) {
          WSI_LOG("wsi_x11_put_image_present #%d: win=0x%x %dx%d depth=%u chunk=%d memTypes=%u type=%d ok=%d conn_err=%d",
                  put_cnt, (unsigned)surf->window, iw, ih, surf->x_depth,
                  (int)(65536u / (uint32_t)(iw * 4)) < 1 ? 1 : (int)(65536u / (uint32_t)(iw * 4)),
                  sc->memProps.memoryTypeCount, type, ok, xcb_connection_has_error(surf->c));
      } }
    if (fence) e->DestroyFence(dev, fence, NULL);
    pFreeCmd(dev, pool, 1, &cb); pDestroyPool(dev, pool, NULL);
    pDestroyBuffer(dev, buf, NULL); sc->FreeMemory(dev, mem, NULL);
    return ok;
}

static int wsi_readback_probe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PVR_WSI_READBACK_PROBE");
        enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return enabled;
}

static VkResult wsi_queue_present(VkQueue queue, const VkPresentInfoKHR *pi) {
    { static int pres_log_n = 0;
      if (pres_log_n < 16 || wsi_verbose_log()) {
          pres_log_n++;
          WSI_LOG("present: enter count=%u sw0=%p image0=%u", pi->swapchainCount,
                  pi->swapchainCount ? (void *)pi->pSwapchains[0] : NULL,
                  pi->swapchainCount ? pi->pImageIndices[0] : 0);
      } }
    /* What the frame was rendered behind: the app makes the queue's rendering wait on a
     * semaphore and presents with it here. The image is about to be read by the compositor, so
     * wait for all wait semaphores to finish first. Waiting with a timeout keeps a semaphore that never
     * signals from wedging the session outright. It must happen before the per-swapchain operations,
     * so all GPU work is synchronized. */
    int fused_wait = 0;
    if (pi->pWaitSemaphores && pi->waitSemaphoreCount > 0 && pi->waitSemaphoreCount <= 16 &&
        pi->swapchainCount == 1 && pi->pSwapchains[0]) {
        struct wsi_swapchain *sc0 = (struct wsi_swapchain *)pi->pSwapchains[0];
        uint32_t idx0 = pi->pImageIndices[0];
        if (sc0 && idx0 < sc0->count && sc0->images[idx0].render != VK_NULL_HANDLE &&
            sc0->surface && (sc0->surface->kind != WSI_X11 || sc0->surface->has_dri3)) {
            fused_wait = 1;
        }
    }
    if (!fused_wait && pi->pWaitSemaphores && pi->waitSemaphoreCount > 0 && pi->swapchainCount > 0) {
        struct wsi_swapchain *sc0 = (struct wsi_swapchain *)pi->pSwapchains[0];
        if (sc0) {
            VkQueue wait_q = queue ? queue : sc0->queue;
            VkResult w = wsi_wait_semaphores(sc0->device, wait_q, pi->waitSemaphoreCount, pi->pWaitSemaphores);
            if (w != VK_SUCCESS) WSI_LOG("present: wait on %u render semaphore(s) failed %d", pi->waitSemaphoreCount, w);
        }
    }
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        struct wsi_swapchain *sc = (struct wsi_swapchain *)pi->pSwapchains[i];
        uint32_t idx = pi->pImageIndices[i];
        if (!sc || idx >= sc->count) { if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR; continue; }
        pthread_mutex_lock(&sc->lock);
        struct wsi_surface *surf = sc->surface;
        if (surf->kind == WSI_X11) {
            if (!surf->has_dri3) {
                static int x11_put_logged;
                if (x11_put_logged < 20) {
                    x11_put_logged++;
                    WSI_LOG("X11 present (PutImage): window=0x%x %dx%d depth=%u has_present=%d",
                            (unsigned)surf->window, sc->width, sc->height, surf->x_depth, surf->has_present);
                }
                int ok = wsi_x11_put_image_present(sc, idx);
                sc->images[idx].acquired = 0;
                pthread_mutex_unlock(&sc->lock);
                if (pi->pResults) pi->pResults[i] = ok ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
                continue;
            }
            /* Give the image's dma-buf to Xwayland as a DRI3 Pixmap (once per image) and show it with
             * Present. buffer_from_pixmap is the X->client direction, not needed here. */
            static int x11_logged;
            if (x11_logged < 40) { x11_logged++;
                WSI_LOG("X11 present: window=0x%x %dx%d stride=%u depth=%u bpp=%u fd=%d (DRI3 PixmapFromBuffers)",
                        (unsigned)surf->window, sc->width, sc->height, sc->stride, surf->x_depth, surf->x_bpp, sc->images[idx].fd); }
            if (sc->images[idx].fd < 0) {
                sc->images[idx].acquired = 0;
                pthread_mutex_unlock(&sc->lock);
                if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR;
                continue;
            }
            if (wsi_readback_probe_enabled()) probe_vk_readback(sc, idx, "x11-render");
            wsi_blit_to_export(sc, idx, queue ? queue : sc->queue,
                               fused_wait ? pi->waitSemaphoreCount : 0,
                               fused_wait ? pi->pWaitSemaphores : NULL);
            if (wsi_readback_probe_enabled()) probe_vk_readback(sc, idx, "x11");
            if (!sc->images[idx].pixmap) {
                xcb_pixmap_t px = xcb_generate_id(surf->c);
                uint32_t size = sc->stride * sc->height;
                xcb_void_cookie_t ck = xcb_dri3_pixmap_from_buffer_checked(surf->c, px, surf->window,
                                             size,
                                             (uint16_t)sc->width, (uint16_t)sc->height,
                                             (uint16_t)sc->stride,
                                             surf->x_depth, surf->x_bpp,
                                             dup(sc->images[idx].fd));
                xcb_generic_error_t *err = xcb_request_check(surf->c, ck);
                if (err) {
                    WSI_LOG("X11 present: DRI3 PixmapFromBuffer failed: error_code=%u major=%u minor=%u "
                            "(window=0x%x %dx%d stride=%u depth=%u size=%u), trying PixmapFromBuffers...",
                            err->error_code, err->major_code, err->minor_code,
                            (unsigned)surf->window, sc->width, sc->height, sc->stride, surf->x_depth, size);
                    free(err);

                    /* Fallback to PixmapFromBuffers with DRM_FORMAT_MOD_INVALID */
                    px = xcb_generate_id(surf->c);
                    int32_t fds[1] = { dup(sc->images[idx].fd) };
                    ck = xcb_dri3_pixmap_from_buffers_checked(surf->c, px, surf->window, 1,
                                                 (uint16_t)sc->width, (uint16_t)sc->height,
                                                 sc->stride, 0, 0, 0, 0, 0, 0, 0,
                                                 surf->x_depth, surf->x_bpp,
                                                 0xffffffffffffffffULL /* DRM_FORMAT_MOD_INVALID */, fds);
                    err = xcb_request_check(surf->c, ck);
                    if (err) {
                        WSI_LOG("X11 present: DRI3 PixmapFromBuffers (MOD_INVALID) failed: error_code=%u major=%u minor=%u",
                                err->error_code, err->major_code, err->minor_code);
                        free(err);

                        /* Fallback to modifier 0 (LINEAR) */
                        px = xcb_generate_id(surf->c);
                        int32_t fds_lin[1] = { dup(sc->images[idx].fd) };
                        ck = xcb_dri3_pixmap_from_buffers_checked(surf->c, px, surf->window, 1,
                                                     (uint16_t)sc->width, (uint16_t)sc->height,
                                                     sc->stride, 0, 0, 0, 0, 0, 0, 0,
                                                     surf->x_depth, surf->x_bpp,
                                                     0 /* LINEAR */, fds_lin);
                        err = xcb_request_check(surf->c, ck);
                        if (err) {
                            WSI_LOG("X11 present: DRI3 PixmapFromBuffers (LINEAR) failed: error_code=%u major=%u minor=%u",
                                    err->error_code, err->major_code, err->minor_code);
                            free(err);
                        } else {
                            sc->images[idx].pixmap = px;
                            WSI_LOG("X11 present: DRI3 PixmapFromBuffers (LINEAR) succeeded! px=0x%x", px);
                        }
                    } else {
                        sc->images[idx].pixmap = px;
                        WSI_LOG("X11 present: DRI3 PixmapFromBuffers (MOD_INVALID) succeeded! px=0x%x", px);
                    }
                } else {
                    sc->images[idx].pixmap = px;
                    WSI_LOG("X11 present: DRI3 PixmapFromBuffer succeeded! px=0x%x", px);
                }
            }
            if (sc->images[idx].pixmap) {
                xcb_present_pixmap(surf->c, surf->window, sc->images[idx].pixmap, ++sc->serial,
                                   XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                                   XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
                xcb_flush(surf->c);
                static int x11_pres_logged;
                if (x11_pres_logged < 5) {
                    x11_pres_logged++;
                    WSI_LOG("X11 present: xcb_present_pixmap ok (conn_err=%d)", xcb_connection_has_error(surf->c));
                }
            }
            sc->images[idx].acquired = 0;
            pthread_mutex_unlock(&sc->lock);
            if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
            continue;
        }
        if (!surf->dmabuf || sc->images[idx].fd < 0) {
            sc->images[idx].acquired = 0;
            pthread_mutex_unlock(&sc->lock);
            if (pi->pResults) pi->pResults[i] = VK_ERROR_SURFACE_LOST_KHR;
            continue;
        }
        /* One wl_buffer per swapchain image, reused for that image's whole life. A wl_buffer
         * describes an fd, a size and a format, all fixed once the image exists, so minting a new
         * one per present only leaks pool entries in the compositor. */
        struct wl_buffer *buf = sc->images[idx].buffer;
        if (!buf) {
            struct zwp_linux_dmabuf_v1 *dmabuf_iface = sc->dmabuf_wrap ? sc->dmabuf_wrap : surf->dmabuf;
            struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf_iface);
            zwp_linux_buffer_params_v1_add(params, sc->images[idx].fd, 0, 0, sc->stride, 0, 0);
            buf = zwp_linux_buffer_params_v1_create_immed(params, sc->width, sc->height, sc->drm_format, 0 /* LINEAR modifier */);
            zwp_linux_buffer_params_v1_destroy(params);
            if (!buf) {
                sc->images[idx].acquired = 0;
                pthread_mutex_unlock(&sc->lock);
                if (pi->pResults) pi->pResults[i] = VK_ERROR_OUT_OF_HOST_MEMORY;
                continue;
            }
            if (sc->wl_queue) wl_proxy_set_queue((struct wl_proxy *)buf, sc->wl_queue);
            wl_buffer_add_listener(buf, &g_buffer_listener, &sc->images[idx]);
            sc->images[idx].buffer = buf;
        }
        { static int rp, last_w, last_h;
          if (rp < 32 || sc->width != last_w || sc->height != last_h) {
              rp++; last_w = sc->width; last_h = sc->height;
              WSI_LOG("present[wl] pid=%d %dx%d cmd=%s", (int)getpid(), sc->width, sc->height, pvr_proc_cmd());
          } }
        if (wsi_readback_probe_enabled()) {
            extern void pvr_icd_cmd_report(const char *, int, int);
            extern void *pvr_icd_last_copy(int);
            extern int pvr_icd_img_info(void *, uint32_t *);
            static int cp;
            if (cp++ < 24) pvr_icd_cmd_report("wl", sc->width, sc->height);
            void *src = pvr_icd_last_copy(0), *dst = pvr_icd_last_copy(1);
            { static int q; if (q++ < 4)
                WSI_LOG("copy pair src=%p dst=%p | mine linear=%p render=%p frac_swapchain=%s",
                        src, dst, (void *)sc->images[idx].image, (void *)sc->images[idx].render,
                        (src == (void *)sc->images[idx].render || src == (void *)sc->images[idx].image) ? "YES" : "no"); }
            if (src && src != (void *)sc->images[idx].image && src != (void *)sc->images[idx].render) {
                uint32_t inf[5] = {0};
                int ext = pvr_icd_img_info(src, inf);
                static int lg; if (lg++ < 4)
                    WSI_LOG("copy src img=%p fmt=%u tiling=%u usage=0x%x %ux%u external=%d",
                            src, inf[0], inf[1], inf[2], inf[3], inf[4], ext);
                if (inf[3] && inf[4]) probe_any_image(sc, (VkImage)src, (int)inf[3], (int)inf[4], "copy-src");
            }
            probe_vk_readback(sc, idx, "wl-render");
        }
        if (sc->wl_queue) {
            wsi_drain_wl_queue(sc, 0);
            for (int waited = 0; sc->images[idx].busy && waited < 100; waited += 20)
                wsi_drain_wl_queue(sc, 20);
        }
        wsi_blit_to_export(sc, idx, queue,
                           fused_wait ? pi->waitSemaphoreCount : 0,
                           fused_wait ? pi->pWaitSemaphores : NULL);
        if (wsi_readback_probe_enabled()) probe_vk_readback(sc, idx, "wl");
        wl_surface_attach(surf->surface, buf, 0, 0);
        wl_surface_damage_buffer(surf->surface, 0, 0, sc->width, sc->height);
        wl_surface_commit(surf->surface);
        wl_display_flush(surf->display);
        sc->images[idx].busy = 1;
        sc->images[idx].acquired = 0;
        if (sc->wl_queue) wsi_drain_wl_queue(sc, 0);
        pthread_mutex_unlock(&sc->lock);
        { static int wl_logged, wl_last_w, wl_last_h;
          if (wl_logged < 20 || sc->width != wl_last_w || sc->height != wl_last_h) {
              wl_logged++; wl_last_w = sc->width; wl_last_h = sc->height;
              WSI_LOG("Wayland present: %dx%d stride=%u fmt=%u fd=%d buffer=%p (wl_buffer attach+commit)",
                      sc->width, sc->height, sc->stride, sc->drm_format, sc->images[idx].fd, (void *)buf);
          } }
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

void *pvr_wsi_sync_proc(const char *name) {
    if (!name) return NULL;
    struct ent { const char *n; void *p; } t[] = {
        { "vkQueueSubmit", (void *)pvr_wsi_queue_submit },
        { "vkQueueSubmit2", (void *)pvr_wsi_queue_submit2 },
        { "vkQueueSubmit2KHR", (void *)pvr_wsi_queue_submit2_khr },
        { "vkQueueWaitIdle", (void *)pvr_wsi_queue_wait_idle },
        { "vkDeviceWaitIdle", (void *)pvr_wsi_device_wait_idle },
        { "vkGetDeviceQueue", (void *)pvr_wsi_get_device_queue },
        { "vkGetDeviceQueue2", (void *)pvr_wsi_get_device_queue2 },
        { "vkDestroyDevice", (void *)pvr_wsi_destroy_device },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(*t); i++) if (!strcmp(name, t[i].n)) return t[i].p;
    return NULL;
}
