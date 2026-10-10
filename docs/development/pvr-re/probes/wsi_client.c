/* wsi_client.c — present a rendered frame to a Wayland compositor through the shim ICD (glibc).
 * The compositor is a separate process (wsi_compositor), reached by an abstract socket. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "tri_spv.h"

#define W 320
#define H 240
#define CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){printf("FAIL %s -> %d\n",#x,_r);_exit(1);} } while(0)
static PFN_vkGetInstanceProcAddr gipa_g;

static struct wl_compositor *g_comp;
static struct zwp_linux_dmabuf_v1 *g_dmb;
static void reg_glob(void *d, struct wl_registry *r, uint32_t n, const char *iface, uint32_t v) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) g_comp = wl_registry_bind(r, n, &wl_compositor_interface, v < 4 ? v : 4);
    else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) g_dmb = wl_registry_bind(r, n, &zwp_linux_dmabuf_v1_interface, v < 4 ? v : 4);
}
static void reg_glob_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== WSI client: render a frame and present it to the compositor ==\n");
    const char *name = getenv("WSI_SOCK"); if (!name) name = "pvr-wl-comp";
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 2, "%s", name);
    socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(addr.sun_path + 1);
    int cfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    for (int i = 0; i < 150 && connect(cfd, (struct sockaddr *)&addr, alen) != 0; i++) usleep(20000);
    struct wl_display *cdpy = wl_display_connect_to_fd(cfd);
    if (!cdpy) { printf("client connect failed\n"); return 2; }
    struct wl_registry *reg = wl_display_get_registry(cdpy);
    struct wl_registry_listener rl; memset(&rl, 0, sizeof(rl));
    rl.global = reg_glob; rl.global_remove = reg_glob_remove;
    wl_registry_add_listener(reg, &rl, NULL);
    int rt = wl_display_roundtrip(cdpy);
    fprintf(stderr, "client: roundtrip=%d comp=%p dmb=%p\n", rt, (void *)g_comp, (void *)g_dmb);
    if (!g_comp || !g_dmb) { printf("client missing globals (comp=%p dmb=%p)\n", (void *)g_comp, (void *)g_dmb); return 2; }
    struct wl_surface *wl_surf = wl_compositor_create_surface(g_comp);
    wl_display_roundtrip(cdpy);
    printf("client wl_surface created\n");

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    gipa_g = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    const char *iext[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "wsi-client"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici2 = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici2.pApplicationInfo = &ai; ici2.enabledExtensionCount = 2; ici2.ppEnabledExtensionNames = iext;
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateInstance)gipa_g(NULL, "vkCreateInstance"))(&ici2, NULL, &inst));
    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);
    uint32_t de = 0;
    ((PFN_vkEnumerateDeviceExtensionProperties)gipa_g(inst, "vkEnumerateDeviceExtensionProperties"))(pd, NULL, &de, NULL);
    printf("device extensions via loader = %u\n", de);

    VkWaylandSurfaceCreateInfoKHR wsci = { VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR };
    wsci.display = cdpy; wsci.surface = wl_surf;
    PFN_vkCreateWaylandSurfaceKHR createWlSurf = (PFN_vkCreateWaylandSurfaceKHR)gipa_g(inst, "vkCreateWaylandSurfaceKHR");
    printf("vkCreateWaylandSurfaceKHR = %s\n", createWlSurf ? "present" : "MISSING");
    if (!createWlSurf) { printf("RESULT: the ICD did not provide the Wayland surface entrypoint\n"); return 1; }
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    CHECK(createWlSurf(inst, &wsci, NULL, &surf));
    VkBool32 supp = 0;
    ((PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa_g(inst, "vkGetPhysicalDeviceSurfaceSupportKHR"))(pd, 0, surf, &supp);
    printf("present supported=%d\n", supp);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *dext[] = { "VK_KHR_swapchain", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 3; dci.ppEnabledExtensionNames = dext;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateDevice)gipa_g(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa_g(inst, "vkGetDeviceProcAddr");
#define P(fn) ((PFN_##fn)gdpa(dev, #fn))

    VkSwapchainCreateInfoKHR scci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    scci.surface = surf; scci.minImageCount = 2; scci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    scci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    scci.imageExtent = (VkExtent2D){ W, H }; scci.imageArrayLayers = 1;
    scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; scci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    scci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; scci.presentMode = VK_PRESENT_MODE_FIFO_KHR; scci.clipped = VK_TRUE;
    PFN_vkCreateSwapchainKHR createSC = P(vkCreateSwapchainKHR);
    printf("vkCreateSwapchainKHR = %s\n", createSC ? "present" : "MISSING");
    if (!createSC) { printf("RESULT: no swapchain entrypoint\n"); return 1; }
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    CHECK(createSC(dev, &scci, NULL, &sc));
    uint32_t imgc = 0; P(vkGetSwapchainImagesKHR)(dev, sc, &imgc, NULL);
    VkImage imgs[4]; P(vkGetSwapchainImagesKHR)(dev, sc, &imgc, imgs);
    printf("swapchain: %u images\n", imgc);

    PFN_vkCreateImageView MVC = P(vkCreateImageView);
    PFN_vkCreateRenderPass MRP = P(vkCreateRenderPass);
    PFN_vkCreateFramebuffer MFB = P(vkCreateFramebuffer);
    PFN_vkCreateShaderModule MSM = P(vkCreateShaderModule);
    PFN_vkCreatePipelineLayout MPL = P(vkCreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines MGP = P(vkCreateGraphicsPipelines);
    PFN_vkCreateCommandPool MCP = P(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers MACB = P(vkAllocateCommandBuffers);
    PFN_vkBeginCommandBuffer MBCB = P(vkBeginCommandBuffer);
    PFN_vkCmdBeginRenderPass MBRP = P(vkCmdBeginRenderPass);
    PFN_vkCmdBindPipeline MCBP = P(vkCmdBindPipeline);
    PFN_vkCmdDraw MCD = P(vkCmdDraw);
    PFN_vkCmdEndRenderPass MCERP = P(vkCmdEndRenderPass);
    PFN_vkEndCommandBuffer MECB = P(vkEndCommandBuffer);
    PFN_vkGetDeviceQueue MGQ = P(vkGetDeviceQueue);
    PFN_vkQueueSubmit MQS = P(vkQueueSubmit);
    PFN_vkQueueWaitIdle MQW = P(vkQueueWaitIdle);

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = imgs[0]; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_B8G8R8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE; CHECK(MVC(dev, &ivci, NULL, &view));
    VkAttachmentDescription att = { 0 };
    att.format = VK_FORMAT_B8G8R8A8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 }; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE; CHECK(MRP(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = W; fci.height = H; fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE; CHECK(MFB(dev, &fci, NULL, &fb));
    VkShaderModuleCreateInfo vsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsci.codeSize = tri_vert_spv_size; vsci.pCode = tri_vert_spv;
    VkShaderModule vs = VK_NULL_HANDLE; CHECK(MSM(dev, &vsci, NULL, &vs));
    VkShaderModuleCreateInfo fsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsci.codeSize = tri_frag_spv_size; fsci.pCode = tri_frag_spv;
    VkShaderModule fs = VK_NULL_HANDLE; CHECK(MSM(dev, &fsci, NULL, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ias = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ias.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D scc = { {0,0}, {W,H} };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scc;
    VkPipelineRasterizationStateCreateInfo rss = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rss.polygonMode = VK_POLYGON_MODE_FILL; rss.cullMode = VK_CULL_MODE_NONE; rss.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo mss = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    mss.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 }; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbs.attachmentCount = 1; cbs.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl = VK_NULL_HANDLE; CHECK(MPL(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ias;
    gp.pViewportState = &vps; gp.pRasterizationState = &rss; gp.pMultisampleState = &mss;
    gp.pColorBlendState = &cbs; gp.layout = pl; gp.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE; CHECK(MGP(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpci.queueFamilyIndex = 0;
    VkCommandPool cp = VK_NULL_HANDLE; CHECK(MCP(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE; CHECK(MACB(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(MBCB(cmd, &cbbi));
    VkClearValue clr = { .color = { {1,0,0,1} } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = (VkRect2D){{0,0},{W,H}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    MBRP(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    MCBP(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    MCD(cmd, 3, 1, 0, 0);
    MCERP(cmd);
    CHECK(MECB(cmd));
    VkQueue q = VK_NULL_HANDLE; MGQ(dev, 0, 0, &q);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    CHECK(MQS(q, 1, &si, VK_NULL_HANDLE));
    CHECK(MQW(q));
    printf("rendered into swapchain image 0\n");

    uint32_t idx = 0;
    P(vkAcquireNextImageKHR)(dev, sc, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkResult pr[1] = { VK_SUCCESS };
    pi.swapchainCount = 1; pi.pSwapchains = &sc; pi.pImageIndices = &idx; pi.pResults = pr;
    VkResult pres = P(vkQueuePresentKHR)(q, &pi);
    printf("vkQueuePresentKHR -> %d (result[0]=%d)\n", pres, pr[0]);
    wl_display_flush(cdpy);
    usleep(200000);
    int ok = (pres == VK_SUCCESS);
    printf("\nRESULT: %s\n", ok ? "the client presented a rendered frame through the ICD's Wayland WSI"
                                 : "present failed");
    fflush(NULL); _exit(ok ? 0 : 1);
}
