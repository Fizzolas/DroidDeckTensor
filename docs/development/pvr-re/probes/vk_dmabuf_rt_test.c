/* vk_dmabuf_rt_test.c — the WSI core: render into an exportable dma-buf, then import that same
 * dma-buf back as a fresh image and verify the rendered pixels survive the round trip.
 *
 * This is exactly what a compositor does with a game's frame: the game renders into a dma-buf-backed
 * image and hands the fd over; the compositor imports the fd and displays it. If the pixels come back
 * correct through the fd, the presentation mechanism works and only the Wayland plumbing remains.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include "tri_spv.h"

#define W 64
#define H 64
#define CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){printf("FAIL %s -> %d\n",#x,_r);_exit(1);} } while(0)
static PFN_vkGetInstanceProcAddr gipa_g;

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== dma-buf round trip (render -> export -> import) through the shim ICD (glibc) ==\n");
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    gipa_g = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vk-rt"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateInstance)gipa_g(NULL, "vkCreateInstance"))(&ci, NULL, &inst));
    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);

    const char *use[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd",
                          "VK_EXT_external_memory_dma_buf", "VK_KHR_dedicated_allocation",
                          "VK_KHR_get_memory_requirements2" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 5; dci.ppEnabledExtensionNames = use;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateDevice)gipa_g(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa_g(inst, "vkGetDeviceProcAddr");
#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))
    DEV(vkCreateImage); DEV(vkGetImageMemoryRequirements); DEV(vkAllocateMemory); DEV(vkBindImageMemory);
    DEV(vkMapMemory); DEV(vkGetMemoryFdKHR); DEV(vkCreateImageView); DEV(vkCreateRenderPass);
    DEV(vkCreateFramebuffer); DEV(vkCreateShaderModule); DEV(vkCreatePipelineLayout); DEV(vkCreateGraphicsPipelines);
    DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers); DEV(vkBeginCommandBuffer); DEV(vkCmdBeginRenderPass);
    DEV(vkCmdBindPipeline); DEV(vkCmdDraw); DEV(vkCmdEndRenderPass); DEV(vkEndCommandBuffer);
    DEV(vkGetDeviceQueue); DEV(vkQueueSubmit); DEV(vkQueueWaitIdle); DEV(vkGetImageSubresourceLayout);

    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa_g(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);

    /* --- image A: exportable color attachment --- */
    VkExternalMemoryImageCreateInfo emiA = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emiA.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.pNext = &emiA; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = (VkExtent3D){ W, H, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_LINEAR; ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage imgA = VK_NULL_HANDLE; CHECK(DEV(vkCreateImage)(dev, &ic, NULL, &imgA));
    VkMemoryRequirements mr; DEV(vkGetImageMemoryRequirements)(dev, imgA, &mr);
    uint32_t mti = 0; for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u<<i)) { mti = i; break; }
    VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedA = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedA.pNext = &expo; dedA.image = imgA;
    VkMemoryAllocateInfo maiA = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiA.pNext = &dedA; maiA.allocationSize = mr.size; maiA.memoryTypeIndex = mti;
    VkDeviceMemory memA = VK_NULL_HANDLE; CHECK(DEV(vkAllocateMemory)(dev, &maiA, NULL, &memA));
    CHECK(DEV(vkBindImageMemory)(dev, imgA, memA, 0));

    /* --- render the triangle into A --- */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = imgA; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE; CHECK(DEV(vkCreateImageView)(dev, &ivci, NULL, &view));
    VkAttachmentDescription att = { 0 };
    att.format = VK_FORMAT_R8G8B8A8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 }; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE; CHECK(DEV(vkCreateRenderPass)(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = W; fci.height = H; fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE; CHECK(DEV(vkCreateFramebuffer)(dev, &fci, NULL, &fb));
    VkShaderModuleCreateInfo vsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsci.codeSize = tri_vert_spv_size; vsci.pCode = tri_vert_spv;
    VkShaderModule vs = VK_NULL_HANDLE; CHECK(DEV(vkCreateShaderModule)(dev, &vsci, NULL, &vs));
    VkShaderModuleCreateInfo fsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsci.codeSize = tri_frag_spv_size; fsci.pCode = tri_frag_spv;
    VkShaderModule fs = VK_NULL_HANDLE; CHECK(DEV(vkCreateShaderModule)(dev, &fsci, NULL, &fs));
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
    VkPipelineLayout pl = VK_NULL_HANDLE; CHECK(DEV(vkCreatePipelineLayout)(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ias;
    gp.pViewportState = &vps; gp.pRasterizationState = &rss; gp.pMultisampleState = &mss;
    gp.pColorBlendState = &cbs; gp.layout = pl; gp.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE; CHECK(DEV(vkCreateGraphicsPipelines)(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; cpci.queueFamilyIndex = 0;
    VkCommandPool cp = VK_NULL_HANDLE; CHECK(DEV(vkCreateCommandPool)(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE; CHECK(DEV(vkAllocateCommandBuffers)(dev, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(DEV(vkBeginCommandBuffer)(cmd, &cbbi));
    VkClearValue clr = { .color = { {0,0,0,1} } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = (VkRect2D){{0,0},{W,H}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    DEV(vkCmdBeginRenderPass)(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    DEV(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    DEV(vkCmdDraw)(cmd, 3, 1, 0, 0);
    DEV(vkCmdEndRenderPass)(cmd);
    CHECK(DEV(vkEndCommandBuffer)(cmd));
    VkQueue q = VK_NULL_HANDLE; DEV(vkGetDeviceQueue)(dev, 0, 0, &q);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    CHECK(DEV(vkQueueSubmit)(q, 1, &si, VK_NULL_HANDLE));
    CHECK(DEV(vkQueueWaitIdle)(q));
    printf("rendered triangle into exportable image A\n");

    /* --- export A's memory as a dma-buf fd --- */
    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = memA; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1; CHECK(DEV(vkGetMemoryFdKHR)(dev, &gfi, &fd));
    printf("exported dma-buf fd=%d\n", fd);

    /* --- image B: import the fd (this is the compositor side) --- */
    VkExternalMemoryImageCreateInfo emiB = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emiB.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo icB = ic; icB.pNext = &emiB; icB.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage imgB = VK_NULL_HANDLE; CHECK(DEV(vkCreateImage)(dev, &icB, NULL, &imgB));
    VkMemoryRequirements mrB; DEV(vkGetImageMemoryRequirements)(dev, imgB, &mrB);
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = fd;
    VkMemoryDedicatedAllocateInfo dedB = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedB.pNext = &imp; dedB.image = imgB;
    VkMemoryAllocateInfo maiB = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiB.pNext = &dedB; maiB.allocationSize = mrB.size; maiB.memoryTypeIndex = mti;
    VkDeviceMemory memB = VK_NULL_HANDLE;
    VkResult br = DEV(vkAllocateMemory)(dev, &maiB, NULL, &memB);
    printf("import dma-buf as memory B -> %d\n", br);
    if (br != VK_SUCCESS) { printf("RESULT: driver could not import its own dma-buf\n"); return 1; }
    CHECK(DEV(vkBindImageMemory)(dev, imgB, memB, 0));

    /* --- verify the imported dma-buf holds the rendered data, via a GPU copy to a host buffer ---
     * (Imported dma-buf memory is not always CPU-mappable, and WSI never maps it: the compositor
     * takes the fd. So read it the way a consumer does -- on the GPU, into host-visible memory.) */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = W * H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer dst = VK_NULL_HANDLE; CHECK(DEV(vkCreateBuffer)(dev, &bci, NULL, &dst));
    VkMemoryRequirements mrD; DEV(vkGetBufferMemoryRequirements)(dev, dst, &mrD);
    uint32_t mtiD = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mrD.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mtiD = i; break; }
    VkMemoryAllocateInfo maiD = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiD.allocationSize = mrD.size; maiD.memoryTypeIndex = mtiD;
    VkDeviceMemory memD = VK_NULL_HANDLE; CHECK(DEV(vkAllocateMemory)(dev, &maiD, NULL, &memD));
    CHECK(DEV(vkBindBufferMemory)(dev, dst, memD, 0));

    DEV(vkCmdPipelineBarrier);
    DEV(vkCmdCopyImageToBuffer);
    VkCommandBufferBeginInfo cbbi2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer cmd2 = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai2.commandPool = cp; cbai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai2.commandBufferCount = 1;
    CHECK(DEV(vkAllocateCommandBuffers)(dev, &cbai2, &cmd2));
    CHECK(DEV(vkBeginCommandBuffer)(cmd2, &cbbi2));
    VkImageMemoryBarrier barr = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barr.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barr.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barr.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barr.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barr.image = imgB; barr.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barr.srcAccessMask = 0; barr.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    DEV(vkCmdPipelineBarrier)(cmd2, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barr);
    VkBufferImageCopy bic = { 0 };
    bic.imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bic.imageExtent = (VkExtent3D){ W, H, 1 };
    DEV(vkCmdCopyImageToBuffer)(cmd2, imgB, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &bic);
    CHECK(DEV(vkEndCommandBuffer)(cmd2));
    VkSubmitInfo si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si2.commandBufferCount = 1; si2.pCommandBuffers = &cmd2;
    CHECK(DEV(vkQueueSubmit)(q, 1, &si2, VK_NULL_HANDLE));
    CHECK(DEV(vkQueueWaitIdle)(q));
    printf("copied imported image -> host buffer\n");

    void *ptr = NULL; CHECK(DEV(vkMapMemory)(dev, memD, 0, VK_WHOLE_SIZE, 0, &ptr));
    uint8_t *base = (uint8_t *)ptr;
    uint8_t *cen = base + (H/2) * (W*4) + (W/2) * 4;
    uint8_t *cor = base + 1 * (W*4) + 1 * 4;
    printf("read via imported dma-buf: centre=%u,%u,%u,%u corner=%u,%u,%u,%u\n",
           cen[0],cen[1],cen[2],cen[3], cor[0],cor[1],cor[2],cor[3]);
    int ok = (cen[0] > 200 && cen[1] < 50) && (cor[0] < 50 && cor[1] < 50);
    printf("\nRESULT: %s\n", ok ? "a rendered frame survives a dma-buf export->import round trip"
                                  : "pixels did not survive the round trip");
    fflush(NULL); _exit(ok ? 0 : 1);
}
