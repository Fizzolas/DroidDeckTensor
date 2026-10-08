/* Compute dispatch through the system Vulkan loader: a SPIR-V shader writes
 * i ^ 0x12345678 into each word of a storage buffer, and the host reads the
 * words back and checks every one. A CPU fill cannot produce that pattern. */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "dispatch_spv.h"

#define WORDS 16384u
#define BUF_BYTES (WORDS * 4u)

static int step(const char *name, VkResult r) {
    printf("%s=%d\n", name, (int)r);
    fflush(stdout);
    return r == VK_SUCCESS;
}

int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    if (!step("instance", vkCreateInstance(&ici, NULL, &inst))) return 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (!step("enum", vkEnumeratePhysicalDevices(inst, &n, &pd)) || n == 0) return 1;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    if (!step("device", vkCreateDevice(pd, &dci, NULL, &dev))) return 1;
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BUF_BYTES,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf;
    if (!step("buffer", vkCreateBuffer(dev, &bci, NULL, &buf))) return 1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
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

    VkDescriptorSetLayoutBinding bind0 = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dslci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = 1, .pBindings = &bind0};
    VkDescriptorSetLayout dsl;
    if (!step("dsl", vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl))) return 1;
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &dsl};
    VkPipelineLayout pl;
    if (!step("pipelayout", vkCreatePipelineLayout(dev, &plci, NULL, &pl))) return 1;

    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = dispatch_spv_bytes, .pCode = dispatch_spv};
    VkShaderModule sm;
    if (!step("shader", vkCreateShaderModule(dev, &smci, NULL, &sm))) return 1;
    VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main"},
                                        .layout = pl};
    VkPipeline pipe;
    if (!step("pipeline", vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe))) return 1;

    VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dp;
    if (!step("dpool", vkCreateDescriptorPool(dev, &dpci, NULL, &dp))) return 1;
    VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    if (!step("dset", vkAllocateDescriptorSets(dev, &dsai, &ds))) return 1;
    VkDescriptorBufferInfo dbi = {.buffer = buf, .offset = 0, .range = BUF_BYTES};
    VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds,
                                .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
    VkCommandPool pool;
    if (!step("pool", vkCreateCommandPool(dev, &pci, NULL, &pool))) return 1;
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    VkCommandBuffer cb;
    if (!step("cmdalloc", vkAllocateCommandBuffers(dev, &cbai, &cb))) return 1;
    VkCommandBufferBeginInfo cbbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (!step("begin", vkBeginCommandBuffer(cb, &cbbi))) return 1;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, WORDS / 64, 1, 1);
    if (!step("end", vkEndCommandBuffer(cb))) return 1;

    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    if (!step("fence", vkCreateFence(dev, &fci, NULL, &fence))) return 1;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    if (!step("submit", vkQueueSubmit(q, 1, &si, fence))) return 1;
    if (!step("wait", vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull))) return 2;

    void *p = NULL;
    if (!step("map", vkMapMemory(dev, mem, 0, mr.size, 0, &p))) return 2;
    const unsigned *w = (const unsigned *)p;
    size_t bad = 0;
    for (unsigned i = 0; i < WORDS; i++) {
        if (w[i] != (i ^ 0x12345678u)) {
            if (bad < 4) printf("  word %u = %08x, want %08x\n", i, w[i], i ^ 0x12345678u);
            bad++;
        }
    }
    printf("shader check: %zu of %u words wrong, first=%08x\n", bad, WORDS, w[0]);
    vkUnmapMemory(dev, mem);
    return bad == 0 ? 0 : 3;
}
