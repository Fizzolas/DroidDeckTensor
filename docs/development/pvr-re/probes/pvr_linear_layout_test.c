/* pvr_linear_layout_test.c - is a VK_IMAGE_TILING_LINEAR export really linear, and complete?
 *
 * The compositor declares modifier 0 (DRM_FORMAT_MOD_LINEAR) to every client and reads the swapped
 * buffer as rows of `pitch` bytes. Measured on the Pixel 10 (2026-10-23, vkcube): 492 of 1080 rows
 * of the pushed dma-buf are ENTIRELY zero, stay zero after settling, and the zero runs repeat with
 * a period of exactly 64 rows. At the HAL's reported rowPitch of 16384, 64 rows is exactly 1 MiB,
 * and the allocation is exactly 32 MiB.
 *
 * The HAL's dma-buf is not CPU-mappable (mmap is refused even to the app uid), so this probe reads
 * the image back THROUGH THE GPU into a host-visible linear image and inspects that. That is the
 * same pixel data the compositor samples, which is the question that matters.
 *
 * If a fully cleared image comes back with zero holes, the export layout is self-consistent and the
 * holes seen in a real frame are the producer's; if the cleared image also has holes, the layout
 * contract is broken at export.
 *
 * Build (WSL):
 *   zig cc -target aarch64-linux-gnu.2.38 -O1 -o pvr_linear_layout_test pvr_linear_layout_test.c \
 *     -I<mesa>/include -ldl
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#define W 2404
#define H 1080
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, _r); _exit(1); } } while (0)

static VkInstance g_inst;
static VkPhysicalDevice g_pd;
static PFN_vkGetInstanceProcAddr g_gipa;

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== is a LINEAR exportable image linear and complete? %dx%d ==\n", W, H);
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("no libvulkan: %s\n", dlerror()); return 2; }
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "pvr-linear"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst; CHECK(((PFN_vkCreateInstance)g_gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst));
    g_inst = inst;
    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)g_gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    if (!nd) { printf("no device\n"); return 1; }
    VkPhysicalDevice pd;
    ((PFN_vkEnumeratePhysicalDevices)g_gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);
    g_pd = pd;

    const char *use[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = use;
    VkDevice dev; CHECK(((PFN_vkCreateDevice)g_gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)g_gipa(inst, "vkGetDeviceProcAddr");

#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))
    PFN_vkCreateImage pCreateImage = DEV(vkCreateImage);
    PFN_vkGetImageMemoryRequirements pGetReqs = DEV(vkGetImageMemoryRequirements);
    PFN_vkAllocateMemory pAlloc = DEV(vkAllocateMemory);
    PFN_vkBindImageMemory pBind = DEV(vkBindImageMemory);
    PFN_vkGetImageSubresourceLayout pLayout = DEV(vkGetImageSubresourceLayout);
    PFN_vkCreateCommandPool pPool = DEV(vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers pCmds = DEV(vkAllocateCommandBuffers);
    PFN_vkBeginCommandBuffer pBegin = DEV(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer pEnd = DEV(vkEndCommandBuffer);
    PFN_vkCmdPipelineBarrier pBarrier = DEV(vkCmdPipelineBarrier);
    PFN_vkCmdClearColorImage pClear = DEV(vkCmdClearColorImage);
    PFN_vkCmdCopyImage pCopy = DEV(vkCmdCopyImage);
    PFN_vkCmdBeginRenderPass pBeginRP = DEV(vkCmdBeginRenderPass);
    PFN_vkCmdEndRenderPass pEndRP = DEV(vkCmdEndRenderPass);
    PFN_vkGetDeviceQueue pQueue = DEV(vkGetDeviceQueue);
    PFN_vkQueueSubmit pSubmit = DEV(vkQueueSubmit);
    PFN_vkQueueWaitIdle pIdle = DEV(vkQueueWaitIdle);
    PFN_vkMapMemory pMap = DEV(vkMapMemory);
    PFN_vkGetPhysicalDeviceMemoryProperties pMemProps =
        (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(inst, "vkGetPhysicalDeviceMemoryProperties");

    VkQueue q; pQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = 0;
    VkCommandPool pool; CHECK(pPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(pCmds(dev, &cbi, &cb));
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };

    /* ---- 1. the production image: LINEAR, exportable, cleared on the GPU ---- */
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ W, H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; CHECK(pCreateImage(dev, &ici, NULL, &img));

    VkMemoryRequirements mr; pGetReqs(dev, img, &mr);
    VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.pNext = &expo; ded.image = img;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &ded; mai.allocationSize = mr.size; mai.memoryTypeIndex = 0;
    for (uint32_t t = 0; t < 32; t++)
        if (mr.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
    VkDeviceMemory mem; CHECK(pAlloc(dev, &mai, NULL, &mem));
    CHECK(pBind(dev, img, mem, 0));

    VkSubresourceLayout lay; memset(&lay, 0, sizeof(lay));
    pLayout(dev, img, &sr, &lay);
    printf("exportable: alloc=%llu offset=%llu rowPitch=%llu (W*4=%d, pitch_*H=%llu)\n",
           (unsigned long long)mr.size, (unsigned long long)lay.offset,
           (unsigned long long)lay.rowPitch, W * 4,
           (unsigned long long)lay.rowPitch * H);

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(pBegin(cb, &bi));
    VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = img; b1.subresourceRange = rr; b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             0, 0, NULL, 0, NULL, 1, &b1);
    VkAttachmentDescription ad; memset(&ad, 0, sizeof(ad));
    ad.format = VK_FORMAT_R8G8B8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sd; memset(&sd, 0, sizeof(sd));
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &ad;
    rpci.subpassCount = 1; rpci.pSubpasses = &sd;
    PFN_vkCreateRenderPass pRP = DEV(vkCreateRenderPass);
    VkRenderPass rp; CHECK(pRP(dev, &rpci, NULL, &rp));

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM; vci.subresourceRange = rr;
    PFN_vkCreateImageView pIV = DEV(vkCreateImageView);
    VkImageView iv; CHECK(pIV(dev, &vci, NULL, &iv));

    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &iv;
    fci.width = W; fci.height = H; fci.layers = 1;
    PFN_vkCreateFramebuffer pFB = DEV(vkCreateFramebuffer);
    VkFramebuffer fb; CHECK(pFB(dev, &fci, NULL, &fb));

    VkClearValue cv; memset(&cv, 0, sizeof(cv));
    cv.color.float32[0] = 1.0f; cv.color.float32[1] = 0.25f;
    cv.color.float32[2] = 0.5f; cv.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb;
    rbi.renderArea.offset.x = 0; rbi.renderArea.offset.y = 0;
    rbi.renderArea.extent.width = W; rbi.renderArea.extent.height = H;
    rbi.clearValueCount = 1; rbi.pClearValues = &cv;
    pBeginRP(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    pEndRP(cb);

    VkImageMemoryBarrier b2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = img; b2.subresourceRange = rr;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             0, 0, NULL, 0, NULL, 1, &b2);

    /* ---- 2. a host-visible linear image to read it back into ---- */
    VkImageCreateInfo rci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    rci.imageType = VK_IMAGE_TYPE_2D; rci.format = VK_FORMAT_R8G8B8A8_UNORM;
    rci.extent = (VkExtent3D){ W, H, 1 }; rci.mipLevels = 1; rci.arrayLayers = 1;
    rci.samples = VK_SAMPLE_COUNT_1_BIT; rci.tiling = VK_IMAGE_TILING_LINEAR;
    rci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; rci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    rci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage rb; CHECK(pCreateImage(dev, &rci, NULL, &rb));
    VkMemoryRequirements rmr; pGetReqs(dev, rb, &rmr);
    VkPhysicalDeviceMemoryProperties mp; pMemProps(pd, &mp);
    int rti = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rmr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { rti = (int)i; break; }
    if (rti < 0) { printf("no host-visible memory type for the readback image\n"); return 1; }
    VkMemoryAllocateInfo rmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    rmai.allocationSize = rmr.size; rmai.memoryTypeIndex = (uint32_t)rti;
    VkDeviceMemory rmem; CHECK(pAlloc(dev, &rmai, NULL, &rmem));
    CHECK(pBind(dev, rb, rmem, 0));
    VkSubresourceLayout rlay; memset(&rlay, 0, sizeof(rlay));
    pLayout(dev, rb, &sr, &rlay);
    void *map = NULL;
    CHECK(pMap(dev, rmem, 0, VK_WHOLE_SIZE, 0, &map));

    VkImageMemoryBarrier rb1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    rb1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; rb1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    rb1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; rb1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    rb1.image = rb; rb1.subresourceRange = rr; rb1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             0, 0, NULL, 0, NULL, 1, &rb1);
    VkImageCopy cp = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    pCopy(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    VkImageMemoryBarrier rb3 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    rb3.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; rb3.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    rb3.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; rb3.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    rb3.image = rb; rb3.subresourceRange = rr;
    rb3.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; rb3.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    pBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
             0, 0, NULL, 0, NULL, 1, &rb3);
    CHECK(pEnd(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    CHECK(pSubmit(q, 1, &si, VK_NULL_HANDLE));
    CHECK(pIdle(q));

    const unsigned char *base = (const unsigned char *)map + rlay.offset;
    int zero_rows = 0, first_zero = -1, last_written = -1;
    int runs[16][2], nruns = 0;
    for (int y = 0; y < H && nruns < 16; ) {
        const unsigned char *r = base + (size_t)y * rlay.rowPitch;
        int nz = 0;
        for (int x = 0; x < W; x++) if (r[x * 4] | r[x * 4 + 1] | r[x * 4 + 2]) nz++;
        if (nz >= W / 100) { last_written = y; y++; continue; }
        int y0 = y;
        while (y < H) {
            const unsigned char *q2 = base + (size_t)y * rlay.rowPitch; int nz2 = 0;
            for (int x = 0; x < W; x++) if (q2[x * 4] | q2[x * 4 + 1] | q2[x * 4 + 2]) nz2++;
            if (nz2 >= W / 100) break;
            y++;
        }
        if (first_zero < 0) first_zero = y0;
        zero_rows += y - y0;
        runs[nruns][0] = y0; runs[nruns][1] = y - y0; nruns++;
    }
    printf("readback rowPitch=%llu\n", (unsigned long long)rlay.rowPitch);
    printf("cleared image, all-zero rows: %d of %d (first %d, last written %d)\n",
           zero_rows, H, first_zero, last_written);
    printf("zero runs:");
    for (int i = 0; i < nruns; i++) printf(" [%d,%d]", runs[i][0], runs[i][1]);
    printf("\nRESULT: %s\n", zero_rows ? "ZERO HOLES in a fully cleared image - the export layout is broken"
                                       : "cleared image is complete - export layout is self-consistent");
    fflush(NULL);
    _exit(zero_rows == 0 ? 0 : 1);
}
