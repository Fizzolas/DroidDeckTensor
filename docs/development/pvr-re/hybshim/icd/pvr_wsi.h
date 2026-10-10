/* pvr_wsi.h — the Wayland WSI the ICD presents: surface + swapchain + present.
 *
 * The vendor HAL exposes only VK_KHR_android_surface, so the guest's Wayland WSI must live here. It
 * talks the Wayland protocol directly (no libwayland): the ICD already has a socket, so it speaks the
 * wire format itself and avoids another shim. A swapchain image is an exportable dma-buf (proven to
 * work); presenting binds it into a wl_buffer via zwp_linux_dmabuf_v1 and attaches that to a surface.
 *
 * Only the Wayland path is here. X11 WSI (for gamescope Xwayland clients) can present through the X11
 * dma-buf path (DRI3) or fall back to Zink; that is a later, separable piece.
 */
#ifndef PVR_WSI_H
#define PVR_WSI_H

#include <vulkan/vulkan.h>

/* Register the WSI entrypoints with the ICD. Called once from the instance setup. */
void pvr_wsi_init(void);

#endif
