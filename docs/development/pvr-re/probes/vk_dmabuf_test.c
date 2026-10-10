/* vk_dmabuf_test.c — can the driver export a displayable dma-buf? (the presentation mechanism)
 *
 * The guest compositor (gamescope/wlroots) takes game frames as linux-dmabuf buffers. So the driver
 * must be able to allocate an image whose backing memory is exportable as a dma-buf fd, ideally with
 * a DRM format modifier. This asks the driver for exactly that, through the shim ICD under glibc:
 *   1. enable VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf;
 *   2. query supported DRM format modifiers for a color attachment (VK_EXT_image_drm_format_modifier);
 *   3. create an image with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
 *   4. allocate exportable memory and get the dma-buf fd;
 *   5. verify the fd is a real dma-buf.
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dlfcn.h>

#define W 64
#define H 64
#define CHECK(x) do { VkResult _r=(x); if(_r!=VK_SUCCESS){printf("FAIL %s -> %d\n",#x,_r);_exit(1);} } while(0)
static PFN_vkGetInstanceProcAddr gipa_g;

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== dma-buf export through the shim ICD (glibc) ==\n");
    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    gipa_g = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vk-dmabuf"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateInstance)gipa_g(NULL, "vkCreateInstance"))(&ci, NULL, &inst));

    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);

    const char *want[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                           "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list" };
    uint32_t en = 0;
    ((PFN_vkEnumerateDeviceExtensionProperties)gipa_g(inst, "vkEnumerateDeviceExtensionProperties"))(pd, NULL, &en, NULL);
    VkExtensionProperties *eps = calloc(en, sizeof(*eps));
    ((PFN_vkEnumerateDeviceExtensionProperties)gipa_g(inst, "vkEnumerateDeviceExtensionProperties"))(pd, NULL, &en, eps);
    const char *use[4] = {0}; int nuse = 0;
    for (int i = 0; i < 4; i++) for (uint32_t k = 0; k < en; k++) if (!strcmp(eps[k].extensionName, want[i])) { use[nuse++] = want[i]; break; }
    printf("enabling %d/%d needed extensions\n", nuse, 4);
    free(eps);
    if (nuse < 2) { printf("RESULT: driver lacks external-memory fd support\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = 0; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)nuse; dci.ppEnabledExtensionNames = use;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateDevice)gipa_g(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa_g(inst, "vkGetDeviceProcAddr");
#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))
    DEV(vkCreateImage); DEV(vkGetImageMemoryRequirements); DEV(vkAllocateMemory); DEV(vkBindImageMemory);
    DEV(vkGetMemoryFdKHR);

    /* Exportable memory for a color attachment. */
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ W, H, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VkResult ir = DEV(vkCreateImage)(dev, &ici, NULL, &img);
    printf("vkCreateImage(exportable, linear) -> %d\n", ir);
    if (ir != VK_SUCCESS) { printf("RESULT: could not create an exportable image\n"); return 1; }

    VkMemoryRequirements mr; DEV(vkGetImageMemoryRequirements)(dev, img, &mr);
    VkExportMemoryAllocateInfo expo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    expo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.pNext = &expo; ded.image = img;
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa_g(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
    uint32_t mti = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) { mti = i; break; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &ded; mai.allocationSize = mr.size; mai.memoryTypeIndex = mti;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult ar = DEV(vkAllocateMemory)(dev, &mai, NULL, &mem);
    printf("vkAllocateMemory(exportable dma-buf) -> %d\n", ar);
    if (ar != VK_SUCCESS) { printf("RESULT: driver cannot allocate exportable memory\n"); return 1; }
    CHECK(DEV(vkBindImageMemory)(dev, img, mem, 0));

    VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    gfi.memory = mem; gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    VkResult fr = DEV(vkGetMemoryFdKHR)(dev, &gfi, &fd);
    printf("vkGetMemoryFdKHR -> %d fd=%d\n", fr, fd);
    if (fr != VK_SUCCESS || fd < 0) { printf("RESULT: could not export a dma-buf fd\n"); return 1; }

    struct stat st; fstat(fd, &st);
    char p[64]; snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
    char link[128] = {0}; ssize_t n = readlink(p, link, sizeof(link)-1);
    if (n > 0) printf("fd -> %s\n", link);
    /* A dma-buf is an anonymous inode named "/dmabuf:"; some kernels report it as a char device. */
    int ok = (strstr(link, "dmabuf") != NULL) || S_ISCHR(st.st_mode);
    printf("fd stat: st_mode=%#o rdev=%#llx\n", st.st_mode, (unsigned long long)st.st_rdev);
    printf("\nRESULT: %s\n", ok ? "the driver exports a displayable dma-buf from a glibc process"
                                  : "exported fd is not a dma-buf");
    fflush(NULL);
    _exit(ok ? 0 : 1);
}
