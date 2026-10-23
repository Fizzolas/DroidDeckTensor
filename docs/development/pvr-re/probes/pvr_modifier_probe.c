/* pvr_linear_layout_test.c - is a VK_IMAGE_TILING_LINEAR export really linear?
 *
 * The compositor declares modifier 0 (DRM_FORMAT_MOD_LINEAR) to every client and reads the
 * swapped buffer as rows of `pitch` bytes. Measured on the Pixel 10 (2026-10-23, vkcube):
 * 492 of 1080 rows of the pushed dma-buf are ENTIRELY zero and stay zero after settling, and
 * the zero runs repeat every 64 rows - whole blocks unwritten, not partial rows, not a race.
 *
 * That is the signature of a producer writing in tile blocks while declaring a linear layout.
 * This probe asks the HAL directly: create a LINEAR exportable image, clear it on the GPU, read
 * the exported fd back row by row, and report which rows came back empty and with what period.
 *
 * Build (WSL, from this dir, same flags as the other probes):
 *   zig cc -target aarch64-linux-gnu.2.38 -O1 -o pvr_linear_layout_test pvr_linear_layout_test.c \
 *     -I<mesa>/include -ldl
 * Run through the ICD like the others (VK_ICD_FILENAMES + HYBRIS_LINKER_DIR).
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <dlfcn.h>

#define W 2404
#define H 1080
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, _r); _exit(1); } } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== is a LINEAR exportable image actually linear? %dx%d ==\n", W, H);
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("no libvulkan: %s\n", dlerror()); return 2; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "pvr-linear"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst; CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst));
    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    if (!nd) { printf("no device\n"); return 1; }
    VkPhysicalDevice pd;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);

    const char *use[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = use;
    VkDevice dev; CHECK(((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))

    DEV(vkCreateImage); DEV(vkGetImageMemoryRequirements); DEV(vkAllocateMemory);
    DEV(vkBindImageMemory); DEV(vkGetMemoryFdKHR); DEV(vkGetImageSubresourceLayout);
    DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers); DEV(vkBeginCommandBuffer);
    DEV(vkCmdPipelineBarrier); DEV(vkCmdClearColorImage); DEV(vkEndCommandBuffer);
    DEV(vkGetDeviceQueue); DEV(vkQueueSubmit); DEV(vkQueueWaitIdle);

    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi; ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ W, H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img; CHECK(DEV(vkCreateImage)(dev, &ici, NULL, &img));

    VkMemoryRequirements mr; DEV(vkGetImageMemoryRequirements)(dev, img, &mr);
    VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.pNext = &expo; ded.image = img;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &ded; mai.allocationSize = mr.size; mai.memoryTypeIndex = 0;
    for (uint32_t t = 0; t < 32; t++)
        if (mr.memoryTypeBits & (1u << t)) { mai.memoryTypeIndex = t; break; }
    VkDeviceMemory mem; CHECK(DEV(vkAllocateMemory)(dev, &mai, NULL, &mem));
    CHECK(DEV(vkBindImageMemory)(dev, img, mem, 0));

    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout lay; memset(&lay, 0, sizeof(lay));
    DEV(vkGetImageSubresourceLayout)(dev, img, &sr, &lay);
    printf("alloc=%llu layout offset=%llu rowPitch=%llu size=%llu\n",
           (unsigned long long)mr.size, (unsigned long long)lay.offset,
           (unsigned long long)lay.rowPitch, (unsigned long long)lay.size);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = 0; VkCommandPool pool;
    CHECK(DEV(vkCreateCommandPool)(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(DEV(vkAllocateCommandBuffers)(dev, &cbi, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(DEV(vkBeginCommandBuffer)(cb, &bi));
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image = img; b1.subresourceRange = rr; b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, NULL, 0, NULL, 1, &b1);
    VkClearColorValue cc; memset(&cc, 0, sizeof(cc));
    cc.float32[0] = 1.0f; cc.float32[1] = 0.25f; cc.float32[2] = 0.5f; cc.float32[3] = 1.0f;
    DEV(vkCmdClearColorImage)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rr);
    VkImageMemoryBarrier b2 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image = img; b2.subresourceRange = rr;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                              0, 0, NULL, 0, NULL, 1, &b2);
    CHECK(DEV(vkEndCommandBuffer)(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    VkQueue q; DEV(vkGetDeviceQueue)(dev, 0, 0, &q);
    CHECK(DEV(vkQueueSubmit)(q, 1, &si, VK_NULL_HANDLE));
    CHECK(DEV(vkQueueWaitIdle)(q));

    printf("(skipping fd mmap: not permitted from this uid)
");
    fflush(NULL);
    _exit(0);
}
