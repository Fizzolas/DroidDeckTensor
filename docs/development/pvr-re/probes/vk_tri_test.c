/* vk_tri_test.c — offscreen graphics: rasterise a red triangle through the shim ICD, under glibc.
 *
 * Renders into an R8G8B8A8_UNORM color image (VK_IMAGE_TILING_LINEAR so it is host-mappable),
 * then reads the pixels back and checks the centre is red (255,0,0) and a corner is black. A pass
 * means the driver does real graphics rasterisation from a glibc process — the same code path a
 * game's rendering goes through. */
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
    printf("== offscreen graphics (triangle) through the shim ICD (glibc) ==\n");
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    gipa_g = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vk-tri"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateInstance)gipa_g(NULL, "vkCreateInstance"))(&ci, NULL, &inst));

    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateDevice)gipa_g(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa_g(inst, "vkGetDeviceProcAddr");
#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))
    DEV(vkCreateImage); DEV(vkGetImageMemoryRequirements); DEV(vkGetImageSubresourceLayout);
    DEV(vkAllocateMemory); DEV(vkBindImageMemory); DEV(vkMapMemory); DEV(vkCreateImageView);
    DEV(vkCreateRenderPass); DEV(vkCreateFramebuffer); DEV(vkCreateShaderModule); DEV(vkCreatePipelineLayout);
    DEV(vkCreateGraphicsPipelines); DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers);
    DEV(vkBeginCommandBuffer); DEV(vkCmdBeginRenderPass); DEV(vkCmdBindPipeline); DEV(vkCmdDraw);
    DEV(vkCmdEndRenderPass); DEV(vkEndCommandBuffer); DEV(vkGetDeviceQueue); DEV(vkQueueSubmit);
    DEV(vkQueueWaitIdle); DEV(vkDeviceWaitIdle);

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ W, H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateImage)(dev, &ici, NULL, &img));
    VkMemoryRequirements mr; DEV(vkGetImageMemoryRequirements)(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa_g(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mti = i; break; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mti;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(DEV(vkAllocateMemory)(dev, &mai, NULL, &mem));
    CHECK(DEV(vkBindImageMemory)(dev, img, mem, 0));
    printf("color image %dx%d created (mem type %u)\n", W, H, mti);

    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = img; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateImageView)(dev, &ivci, NULL, &view));

    VkAttachmentDescription att = { 0 };
    att.format = VK_FORMAT_R8G8B8A8_UNORM; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0 }; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateRenderPass)(dev, &rpci, NULL, &rp));

    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = rp; fci.attachmentCount = 1; fci.pAttachments = &view; fci.width = W; fci.height = H; fci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateFramebuffer)(dev, &fci, NULL, &fb));

    VkShaderModuleCreateInfo vsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsci.codeSize = tri_vert_spv_size; vsci.pCode = tri_vert_spv;
    VkShaderModule vs = VK_NULL_HANDLE; CHECK(DEV(vkCreateShaderModule)(dev, &vsci, NULL, &vs));
    VkShaderModuleCreateInfo fsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsci.codeSize = tri_frag_spv_size; fsci.pCode = tri_frag_spv;
    VkShaderModule fs = VK_NULL_HANDLE; CHECK(DEV(vkCreateShaderModule)(dev, &fsci, NULL, &fs));

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { {0,0}, {W,H} };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl = VK_NULL_HANDLE; CHECK(DEV(vkCreatePipelineLayout)(dev, &plci, NULL, &pl));

    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = stages; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb; gp.layout = pl; gp.renderPass = rp; gp.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateGraphicsPipelines)(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    printf("graphics pipeline ok\n");

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = 0;
    VkCommandPool cp = VK_NULL_HANDLE; CHECK(DEV(vkCreateCommandPool)(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE; CHECK(DEV(vkAllocateCommandBuffers)(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(DEV(vkBeginCommandBuffer)(cmd, &cbbi));
    VkClearValue clear = { .color = { {0,0,0,1} } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = rp; rbi.framebuffer = fb; rbi.renderArea = (VkRect2D){{0,0},{W,H}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    DEV(vkCmdBeginRenderPass)(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    DEV(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    DEV(vkCmdDraw)(cmd, 3, 1, 0, 0);
    DEV(vkCmdEndRenderPass)(cmd);
    CHECK(DEV(vkEndCommandBuffer)(cmd));

    VkQueue q = VK_NULL_HANDLE; DEV(vkGetDeviceQueue)(dev, 0, 0, &q);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    CHECK(DEV(vkQueueSubmit)(q, 1, &si, VK_NULL_HANDLE));
    CHECK(DEV(vkQueueWaitIdle)(q));
    printf("rendered; reading back...\n");

    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    DEV(vkGetImageSubresourceLayout)(dev, img, &sr, &layout);
    void *ptr = NULL;
    CHECK(DEV(vkMapMemory)(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr));
    uint8_t *base = (uint8_t *)ptr + layout.offset;
    /* centre pixel and corner pixel */
    uint8_t *cen = base + (H/2) * layout.rowPitch + (W/2) * 4;
    uint8_t *cor = base + 1 * layout.rowPitch + 1 * 4;
    printf("centre RGBA=%u,%u,%u,%u   corner RGBA=%u,%u,%u,%u   (rowPitch=%llu)\n",
           cen[0],cen[1],cen[2],cen[3], cor[0],cor[1],cor[2],cor[3], (unsigned long long)layout.rowPitch);
    int ok = (cen[0] > 200 && cen[1] < 50 && cen[2] < 50) && (cor[0] < 50 && cor[1] < 50 && cor[2] < 50);
    printf("\nRESULT: %s\n", ok ? "the PowerVR GPU rasterised a red triangle from a glibc process"
                                : "unexpected pixels");
    fflush(NULL);
    _exit(ok ? 0 : 1);
}
