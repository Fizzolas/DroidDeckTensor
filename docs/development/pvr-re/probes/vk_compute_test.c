/* vk_compute_test.c — run real GPU work through the shim ICD, under glibc.
 *
 * Uses the official Vulkan headers and the classic loader pattern (vkGetInstanceProcAddr from the
 * *loader*, then vkGetDeviceProcAddr for device functions). Builds a storage buffer, dispatches a
 * compute shader that writes data[i] = i*3 + 7, reads it back, and verifies. A correct result means
 * the driver is not just enumerating but executing on the GPU.
 *
 * Build (glibc): zig cc -target aarch64-linux-gnu -I<VulkanSDK>/Include vk_compute_test.c -L<dir> -ldl
 * Env: VK_ICD_FILENAMES, HYBRIS_LINKER_DIR, HYBRIS_LD_LIBRARY_PATH.
 */
#define VK_USE_PLATFORM_ANDROID_KHR 0
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pattern_spv.h"

#define N 256
#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { printf("FAIL %s -> %d\n", #x, _r); _exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa_g;

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== GPU compute through the shim ICD (glibc) ==\n");

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("dlopen failed: %s\n", dlerror()); return 2; }
    gipa_g = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "vk-compute"; ai.applicationVersion = 1; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &ai;
    VkInstance inst = VK_NULL_HANDLE;
    CHECK(gipa_g(NULL, "vkCreateInstance") ? ((PFN_vkCreateInstance)gipa_g(NULL, "vkCreateInstance"))(&ci, NULL, &inst) : VK_ERROR_INITIALIZATION_FAILED);

    uint32_t nd = 0;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, NULL);
    if (!nd) { printf("no devices\n"); return 1; }
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    ((PFN_vkEnumeratePhysicalDevices)gipa_g(inst, "vkEnumeratePhysicalDevices"))(inst, &nd, &pd);

    uint32_t nq = 0;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa_g(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(pd, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa_g(inst, "vkGetPhysicalDeviceQueueFamilyProperties"))(pd, &nq, qf);
    uint32_t qf_compute = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) || (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qf_compute = i;
    printf("queue family for compute: %u\n", qf_compute);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qf_compute; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    CHECK(((PFN_vkCreateDevice)gipa_g(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev));
    printf("vkCreateDevice ok\n");

    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa_g(inst, "vkGetDeviceProcAddr");
#define DEV(fn) ((PFN_##fn)gdpa(dev, #fn))
    DEV(vkCreateBuffer); DEV(vkGetBufferMemoryRequirements); DEV(vkAllocateMemory); DEV(vkBindBufferMemory);
    DEV(vkMapMemory); DEV(vkCreateShaderModule); DEV(vkCreateDescriptorSetLayout); DEV(vkCreatePipelineLayout);
    DEV(vkCreateComputePipelines); DEV(vkCreateDescriptorPool); DEV(vkAllocateDescriptorSets);
    DEV(vkUpdateDescriptorSets); DEV(vkCreateCommandPool); DEV(vkAllocateCommandBuffers); DEV(vkBeginCommandBuffer);
    DEV(vkCmdBindPipeline); DEV(vkCmdBindDescriptorSets); DEV(vkCmdDispatch); DEV(vkEndCommandBuffer);
    DEV(vkQueueSubmit); DEV(vkQueueWaitIdle); DEV(vkGetDeviceQueue);

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = N * sizeof(uint32_t); bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateBuffer)(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; DEV(vkGetBufferMemoryRequirements)(dev, buf, &mr);
    /* Pick a host-visible, host-coherent memory type the buffer allows. */
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa_g(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
    uint32_t mti = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(mr.memoryTypeBits & (1u << i))) continue;
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mti = i; break; }
    }
    if (mti == UINT32_MAX) for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mti = i; break; }
    printf("memory: %u types, chose index %u (bits=%#x)\n", mp.memoryTypeCount, mti, mr.memoryTypeBits);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = mti;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    CHECK(DEV(vkAllocateMemory)(dev, &mai, NULL, &mem));
    CHECK(DEV(vkBindBufferMemory)(dev, buf, mem, 0));
    printf("buffer+memory ok (allocated %llu bytes)\n", (unsigned long long)mr.size);

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = pattern_spv_size; smci.pCode = pattern_spv;
    VkShaderModule sm = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateShaderModule)(dev, &smci, NULL, &sm));

    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1; dslci.pBindings = &b;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateDescriptorSetLayout)(dev, &dslci, NULL, &dsl));

    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    CHECK(DEV(vkCreatePipelineLayout)(dev, &plci, NULL, &pl));

    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main";
    cpci.layout = pl;
    VkPipeline pipe = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateComputePipelines)(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
    printf("compute pipeline ok\n");

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateDescriptorPool)(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = dp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    CHECK(DEV(vkAllocateDescriptorSets)(dev, &dsai, &ds));

    VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = ds; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi;
    DEV(vkUpdateDescriptorSets)(dev, 1, &w, 0, NULL);

    VkCommandPoolCreateInfo cpoci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpoci.queueFamilyIndex = qf_compute;
    VkCommandPool cp = VK_NULL_HANDLE;
    CHECK(DEV(vkCreateCommandPool)(dev, &cpoci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = cp; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    CHECK(DEV(vkAllocateCommandBuffers)(dev, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(DEV(vkBeginCommandBuffer)(cmd, &cbbi));
    DEV(vkCmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    DEV(vkCmdBindDescriptorSets)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    DEV(vkCmdDispatch)(cmd, N / 64, 1, 1);
    CHECK(DEV(vkEndCommandBuffer)(cmd));

    VkQueue q = VK_NULL_HANDLE;
    DEV(vkGetDeviceQueue)(dev, qf_compute, 0, &q);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    CHECK(DEV(vkQueueSubmit)(q, 1, &si, VK_NULL_HANDLE));
    printf("submitted; waiting...\n");
    CHECK(DEV(vkQueueWaitIdle)(q));
    printf("queue idle\n");

    void *ptr = NULL;
    CHECK(DEV(vkMapMemory)(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr));
    uint32_t *out = (uint32_t *)ptr;
    int bad = 0;
    for (int i = 0; i < N; i++) if (out[i] != (uint32_t)(i * 3 + 7)) { if (bad < 5) printf("  mismatch[%d]=%u want %u\n", i, out[i], i*3+7); bad++; }
    printf("\n%s: %d/%d values correct\n", bad ? "FAIL" : "SUCCESS", N - bad, N);
    printf("RESULT: %s\n", bad ? "GPU compute did NOT produce correct output" : "the PowerVR GPU executed a compute shader from a glibc process");
    fflush(NULL);
    _exit(bad ? 1 : 0);
}
