/* Smallest end-to-end Vulkan proof: instance, device, buffer, empty command
 * buffer, queue submit, fence wait. Built against the NDK stub, linked at run
 * time to the system loader. Prints each step's VkResult. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

static int step(const char *name, VkResult r) {
    printf("%s=%d\n", name, (int)r);
    fflush(stdout);
    return r == VK_SUCCESS;
}

int main(int argc, char **argv) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (!step("instance", vkCreateInstance(&ici, NULL, &inst))) return 1;

    uint32_t n = 0;
    VkPhysicalDevice pd[4];
    step("enum", vkEnumeratePhysicalDevices(inst, &n, NULL));
    n = 1;
    step("enum2", vkEnumeratePhysicalDevices(inst, &n, pd));
    if (n == 0) return 1;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &qn, NULL);
    VkQueueFamilyProperties qp[8];
    if (qn > 8) qn = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(pd[0], &qn, qp);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    if (!step("device", vkCreateDevice(pd[0], &dci, NULL, &dev))) return 1;
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    if (!step("buffer", vkCreateBuffer(dev, &bci, NULL, &buf))) return 1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd[0], &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            mt = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = mr.size, .memoryTypeIndex = mt};
    VkDeviceMemory mem;
    if (!step("alloc", vkAllocateMemory(dev, &mai, NULL, &mem))) return 1;
    if (!step("bind", vkBindBufferMemory(dev, buf, mem, 0))) return 1;

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool;
    if (!step("pool", vkCreateCommandPool(dev, &pci, NULL, &pool))) return 1;
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer cb;
    if (!step("cmdalloc", vkAllocateCommandBuffers(dev, &cbai, &cb))) return 1;
    int fill = argc > 1 && strcmp(argv[1], "fill") == 0;
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!step("begin", vkBeginCommandBuffer(cb, &cbbi))) return 1;
    if (fill) vkCmdFillBuffer(cb, buf, 0, 65536, 0x5a5a5a5au);
    if (!step("end", vkEndCommandBuffer(cb))) return 1;

    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    if (!step("fence", vkCreateFence(dev, &fci, NULL, &fence))) return 1;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (!step("submit", vkQueueSubmit(q, 1, &si, fence))) return 1;
    int ok = step("wait", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
    printf("result=%s\n", ok ? "fence signalled" : "no signal");
    if (ok && fill) {
        void *p = NULL;
        if (!step("map", vkMapMemory(dev, mem, 0, mr.size, 0, &p))) return 2;
        const unsigned *w = (const unsigned *)p;
        size_t bad = 0, n = 65536 / 4;
        for (size_t i = 0; i < n; i++) if (w[i] != 0x5a5a5a5au) bad++;
        printf("fill check: %zu of %zu words wrong, first=%08x\n", bad, n, w[0]);
        vkUnmapMemory(dev, mem);
        return bad == 0 ? 0 : 3;
    }
    return ok ? 0 : 2;
}
