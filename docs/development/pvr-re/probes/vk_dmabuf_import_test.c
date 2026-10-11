/* vk_dmabuf_import_test.c - the compositor's side of the WSI hand-off, isolated.
 *
 * The round-trip test (vk_dmabuf_rt_test.c) proves the driver can export a dma-buf and re-import it
 * in the same process, in the same format and tiling it was created with. gamescope's linux-dmabuf
 * import does two things that test does not, and one of them is what it rejects our frames over:
 *
 *   1. it imports a VK_FORMAT_B8G8R8A8_UNORM buffer - the format the WSI swapchain uses (VK fmt 44),
 *      not the R8G8B8A8 the round-trip test used;
 *   2. it queries the fd's properties with vkGetMemoryFdPropertiesKHR before importing, which the
 *      round-trip test never calls.
 *
 * So this exports a BGRA8 LINEAR image exactly as pvr_wsi.c does, then imports it the way a
 * compositor would. A failure here is the "Failed to import DMA-BUF FD for plane 0: Invalid
 * argument" that gamescope logs, reproduced in isolation.
 *
 * Build: zig cc -target aarch64-linux-gnu.2.38 -O1 -I<VulkanSDK>/Include -o vk_dmabuf_import_test ...
 * Run: same glibc staging as the other probes (see README.md).
 */
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define W 64
#define H 64
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { printf("FAIL %s -> %d\n", #x, _r); return 1; } } while (0)

/* Resolved through the loader, like every other probe here (no link-time Vulkan symbols). */
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
#define DEV(f) ((PFN_##f)gdpa(dev, #f))
#define GIPA(name) ((PFN_##name)gipa(inst, #name))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: a fault must not swallow the trace */
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("no libvulkan.so.1\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no gipa\n"); return 1; }

    VkInstance inst;
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));

    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)GIPA(vkEnumeratePhysicalDevices))(inst, &n, &pd) != VK_SUCCESS || n < 1) {
        printf("no device\n"); return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    CHECK(((PFN_vkCreateDevice)GIPA(vkCreateDevice))(pd, &dci, NULL, &dev));
    gdpa = (PFN_vkGetDeviceProcAddr)GIPA(vkGetDeviceProcAddr);
    if (!gdpa) { printf("no device gdpa\n"); return 1; }

    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)GIPA(vkGetPhysicalDeviceMemoryProperties))(pd, &mp);

    /* --- image A: exportable, BGRA8, LINEAR - exactly what pvr_wsi.c creates for the swapchain --- */
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ic.pNext = &emi; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent = (VkExtent3D){ W, H, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_LINEAR;
    ic.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage imgA;
    CHECK(DEV(vkCreateImage)(dev, &ic, NULL, &imgA));
    VkMemoryRequirements mr;
    DEV(vkGetImageMemoryRequirements)(dev, imgA, &mr);
    uint32_t mti = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) { mti = i; break; }
    VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo dedA = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedA.pNext = &expo; dedA.image = imgA;
    VkMemoryAllocateInfo maiA = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiA.pNext = &dedA; maiA.allocationSize = mr.size; maiA.memoryTypeIndex = mti;
    VkDeviceMemory memA;
    CHECK(DEV(vkAllocateMemory)(dev, &maiA, NULL, &memA));
    CHECK(DEV(vkBindImageMemory)(dev, imgA, memA, 0));

    VkImageSubresourceLayers sl = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    VkSubresourceLayout lay; memset(&lay, 0, sizeof(lay));
    DEV(vkGetImageSubresourceLayout)(dev, imgA, &sl, &lay);
    printf("exported image: BGRA8 LINEAR %dx%d rowPitch=%llu offset=%llu\n",
           W, H, (unsigned long long)lay.rowPitch, (unsigned long long)lay.offset);

    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = memA; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    CHECK(DEV(vkGetMemoryFdKHR)(dev, &gfi, &fd));
    printf("exported dma-buf fd=%d\n", fd);

    /* --- step 1: the compositor queries the fd's properties (gamescope does; round-trip does not) --- */
    VkMemoryFdPropertiesKHR fdp = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    VkResult pr = DEV(vkGetMemoryFdPropertiesKHR)
        ? DEV(vkGetMemoryFdPropertiesKHR)(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp)
        : VK_ERROR_EXTENSION_NOT_PRESENT;
    printf("vkGetMemoryFdPropertiesKHR -> %d  memoryTypeBits=0x%x\n", pr, fdp.memoryTypeBits);
    if (pr != VK_SUCCESS) {
        printf("RESULT: compositor cannot query the fd's properties - this is the import failure\n");
        return 2;
    }

    /* --- step 2: import as a fresh BGRA8 LINEAR image, the compositor's image --- */
    VkExternalMemoryImageCreateInfo emiB = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emiB.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo icB = ic; icB.pNext = &emiB;
    icB.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage imgB;
    CHECK(DEV(vkCreateImage)(dev, &icB, NULL, &imgB));
    VkMemoryRequirements mrB;
    DEV(vkGetImageMemoryRequirements)(dev, imgB, &mrB);
    /* The compositor picks a memory type the fd allows. */
    uint32_t mtiB = 0; int found = 0;
    for (uint32_t i = 0; i < 32; i++)
        if ((mrB.memoryTypeBits & (1u << i)) && (fdp.memoryTypeBits & (1u << i))) { mtiB = i; found = 1; break; }
    if (!found) for (uint32_t i = 0; i < 32; i++) if (mrB.memoryTypeBits & (1u << i)) { mtiB = i; break; }
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = fd;
    VkMemoryDedicatedAllocateInfo dedB = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedB.pNext = &imp; dedB.image = imgB;
    VkMemoryAllocateInfo maiB = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiB.pNext = &dedB; maiB.allocationSize = mrB.size; maiB.memoryTypeIndex = mtiB;
    VkDeviceMemory memB;
    VkResult br = DEV(vkAllocateMemory)(dev, &maiB, NULL, &memB);
    printf("import BGRA8 dma-buf as memory B (type %u) -> %d\n", mtiB, br);
    if (br != VK_SUCCESS) { printf("RESULT: BGRA8 dma-buf import FAILS - reproduced gamescope's error\n"); return 3; }
    CHECK(DEV(vkBindImageMemory)(dev, imgB, memB, 0));

    printf("\nRESULT: a BGRA8 LINEAR dma-buf imports as a plain LINEAR image\n");

    /* --- step 2b: import the SAME fd into an OPTIMAL-tiling image. gamescope's renderer imports a
     * compositor buffer with createFlags {bSampled,bTransferSrc} and neither bMappable nor bLinear,
     * so its tiling is OPTIMAL (rendervulkan.cpp:2102), not LINEAR. If the HAL can only bind imported
     * memory to LINEAR images, this is the "vkBindImageMemory failed" gamescope logs. --- */
    VkExternalMemoryImageCreateInfo emiB2 = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emiB2.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo icB2 = ic; icB2.pNext = &emiB2; icB2.tiling = VK_IMAGE_TILING_OPTIMAL;
    icB2.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkImage imgB2;
    VkResult crB2 = DEV(vkCreateImage)(dev, &icB2, NULL, &imgB2);
    printf("import fd into an OPTIMAL-tiling image -> vkCreateImage=%d\n", crB2);
    if (crB2 == VK_SUCCESS) {
        VkMemoryRequirements mrB2;
        printf("  calling vkGetImageMemoryRequirements on the OPTIMAL image...\n");
        DEV(vkGetImageMemoryRequirements)(dev, imgB2, &mrB2);
        printf("  -> size=%llu typeBits=0x%x\n", (unsigned long long)mrB2.size, mrB2.memoryTypeBits);
        uint32_t mtiB2 = 0; int f2 = 0;
        for (uint32_t i = 0; i < 32; i++)
            if ((mrB2.memoryTypeBits & (1u << i)) && (fdp.memoryTypeBits & (1u << i))) { mtiB2 = i; f2 = 1; break; }
        if (!f2) for (uint32_t i = 0; i < 32; i++) if (mrB2.memoryTypeBits & (1u << i)) { mtiB2 = i; break; }
        VkImportMemoryFdInfoKHR imp2 = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
        imp2.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp2.fd = fd;
        VkMemoryDedicatedAllocateInfo ded2 = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        ded2.pNext = &imp2; ded2.image = imgB2;
        VkMemoryAllocateInfo mai2 = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai2.pNext = &ded2; mai2.allocationSize = mrB2.size; mai2.memoryTypeIndex = mtiB2;
        VkDeviceMemory memB2;
        VkResult ar2 = DEV(vkAllocateMemory)(dev, &mai2, NULL, &memB2);
        printf("  vkAllocateMemory (type %u) = %d\n", mtiB2, ar2);
        if (ar2 == VK_SUCCESS) {
            VkResult br2 = DEV(vkBindImageMemory)(dev, imgB2, memB2, 0);
            printf("  vkBindImageMemory = %d\n", br2);
            if (br2 != VK_SUCCESS) { printf("RESULT: OPTIMAL-tiling import BINDS FAIL - gamescope's error\n"); return 6; }
            printf("RESULT: OPTIMAL-tiling import works too\n");
        }
    }

    /* --- step 3: import the SAME fd the way gamescope actually does - as a DRM-format-modifier image.
     * A compositor's Vulkan renderer does not import a LINEAR-tiling image; it imports with
     * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and names the modifier explicitly (0 = LINEAR for us).
     * If the HAL's import only works for LINEAR tiling, this is where "Invalid argument" comes from. */
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
    mod.drmFormatModifier = 0;   /* LINEAR */
    mod.drmFormatModifierPlaneCount = 1;
    VkSubresourceLayout plane = { 0 };
    plane.rowPitch = lay.rowPitch; plane.offset = lay.offset;
    mod.pPlaneLayouts = &plane;
    VkExternalMemoryImageCreateInfo emiC = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emiC.pNext = &mod; emiC.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo icC = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    icC.pNext = &emiC; icC.imageType = VK_IMAGE_TYPE_2D; icC.format = VK_FORMAT_B8G8R8A8_UNORM;
    icC.extent = (VkExtent3D){ W, H, 1 }; icC.mipLevels = 1; icC.arrayLayers = 1;
    icC.samples = VK_SAMPLE_COUNT_1_BIT; icC.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    icC.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    icC.sharingMode = VK_SHARING_MODE_EXCLUSIVE; icC.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage imgC;
    VkResult cr = DEV(vkCreateImage)(dev, &icC, NULL, &imgC);
    printf("import fd as DRM_FORMAT_MODIFIER image (modifier 0) -> vkCreateImage=%d\n", cr);
    if (cr != VK_SUCCESS) { printf("RESULT: DRM_FORMAT_MODIFIER import image rejected - gamescope's error\n"); return 4; }
    VkMemoryRequirements mrC;
    printf("  calling vkGetImageMemoryRequirements on the modifier image...\n");
    DEV(vkGetImageMemoryRequirements)(dev, imgC, &mrC);
    printf("  -> size=%llu alignment=%llu memoryTypeBits=0x%x\n",
           (unsigned long long)mrC.size, (unsigned long long)mrC.alignment, mrC.memoryTypeBits);
    VkImportMemoryFdInfoKHR impC = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    impC.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; impC.fd = fd;
    VkMemoryDedicatedAllocateInfo dedC = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedC.pNext = &impC; dedC.image = imgC;
    VkMemoryAllocateInfo maiC = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiC.pNext = &dedC; maiC.allocationSize = mrC.size; maiC.memoryTypeIndex = mtiB;
    VkDeviceMemory memC;
    uint32_t mtiC = mtiB;
    for (uint32_t i = 0; i < 32; i++) if (mrC.memoryTypeBits & (1u << i)) { mtiC = i; break; }
    maiC.memoryTypeIndex = mtiC;
    printf("  calling vkAllocateMemory (import fd, dedicated, type %u of 0x%x, size %llu)...\n",
           mtiC, mrC.memoryTypeBits, (unsigned long long)maiC.allocationSize);
    VkResult cr2 = DEV(vkAllocateMemory)(dev, &maiC, NULL, &memC);
    printf("import fd as DRM_FORMAT_MODIFIER memory -> vkAllocateMemory=%d\n", cr2);
    if (cr2 != VK_SUCCESS) { printf("RESULT: DRM_FORMAT_MODIFIER memory import FAILS - gamescope's error\n"); return 5; }
    CHECK(DEV(vkBindImageMemory)(dev, imgC, memC, 0));

    printf("\nRESULT: a BGRA8 dma-buf imports as a DRM_FORMAT_MODIFIER image too - the compositor path\n");
    return 0;
}
