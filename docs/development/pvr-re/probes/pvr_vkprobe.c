/*
 * pvr_vkprobe.c — Vulkan capability probe for the PowerVR ICD
 *
 * Purpose: record the ground truth of what /vendor/lib64/hw/vulkan.powervr.so
 * actually exposes on a Pixel 10 (Tensor G5 / PowerVR D-Series DXT-48-1536),
 * which decides what a DroidDeck session can and cannot do on this device.
 *
 * Static-linked against libvulkan.so (the system loader) and dlopens nothing
 * itself; the loader driver-loads the ICD from the on-device discovery
 * (/vendor/etc/... or the system default). We only ask the instance.
 *
 * No Vulkan headers required: minimal decls are defined locally. AArch64,
 * little-endian, pointer sizes for 64-bit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int32_t  VkResult;
typedef uint32_t VkFlags;
typedef uint64_t VkDeviceSize;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef void* (*PFN_vkVoidFunction)(void);

#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 1000059000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 1000059001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES 1000096000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES 1000096001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES 1000196000
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES 1000096002
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES 1000096003
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES 1000196001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES 1000196000
#define VK_MAKE_API_VERSION(v,variant,major,minor) ((((uint32_t)(v))<<29)|(((uint32_t)(variant))<<22)|(((uint32_t)(major))<<12)|((uint32_t)(minor)))
#define VK_API_VERSION_VARIANT(v) ((uint32_t)(v)>>29)
#define VK_API_VERSION_MAJOR(v) (((uint32_t)(v)>>22)&0x7FU)
#define VK_API_VERSION_MINOR(v) (((uint32_t)(v)>>12)&0x3FFU)

typedef struct VkApplicationInfo {
    uint32_t sType; const void* pNext; const char* pApplicationName;
    uint32_t applicationVersion; const char* pEngineName; uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
    uint32_t sType; const void* pNext; VkFlags flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct { uint32_t sType; const void* pNext; VkFlags flags; } VkExtBase;

typedef struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;

typedef struct VkPhysicalDeviceProperties {
    uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID;
    uint32_t deviceType; char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    struct { uint32_t maxImageDimension1D,maxImageDimension2D,maxImageDimension3D,maxImageDimensionCube;
             uint32_t maxImageArrayLayers,maxTexelBufferElements,maxUniformBufferRange,maxStorageBufferRange;
             uint32_t maxPushConstantsSize,maxMemoryAllocationCount,maxSamplerAllocationCount;
             VkDeviceSize bufferImageGranularity,sparseAddressSpaceSize;
             uint32_t maxBoundDescriptorSets,maxPerStageDescriptorSamplers,maxPerStageDescriptorUniformBuffers;
             uint32_t maxPerStageDescriptorStorageBuffers,maxPerStageDescriptorSampledImages;
             uint32_t maxPerStageDescriptorStorageImages,maxPerStageDescriptorInputAttachments;
             uint32_t maxPerStageResources,maxDescriptorSetSamplers,maxDescriptorSetUniformBuffers;
             uint32_t maxDescriptorSetUniformBuffersDynamic,maxDescriptorSetStorageBuffers;
             uint32_t maxDescriptorSetStorageBuffersDynamic,maxDescriptorSetSampledImages;
             uint32_t maxDescriptorSetStorageImages,maxDescriptorSetInputAttachments;
             uint32_t maxVertexInputAttributes,maxVertexInputBindings,maxVertexInputAttributeOffset;
             uint32_t maxVertexInputBindingStride,maxVertexOutputComponents;
             uint32_t maxTessellationGenerationLevel,maxTessellationPatchSize;
             uint32_t maxTessellationControlPerVertexInputComponents,maxTessellationControlPerVertexOutputComponents;
             uint32_t maxTessellationControlPerPatchOutputComponents,maxTessellationControlTotalOutputComponents;
             uint32_t maxTessellationEvaluationInputComponents,maxTessellationEvaluationOutputComponents;
             uint32_t maxGeometryShaderInvocations,maxGeometryInputComponents,maxGeometryOutputComponents;
             uint32_t maxGeometryOutputVertices,maxGeometryTotalOutputComponents,maxGeometryOutputComponents2;
             uint32_t maxFragmentInputComponents,maxFragmentOutputAttachments,maxFragmentDualSrcAttachments;
             uint32_t maxFragmentCombinedOutputResources;
             uint32_t maxComputeSharedMemorySize;
             uint32_t maxComputeWorkGroupCount[3],maxComputeWorkGroupInvocations,maxComputeWorkGroupSize[3];
             uint32_t subPixelPrecisionBits,subTexelPrecisionBits,mipmapPrecisionBits;
             uint32_t maxDrawIndexedIndexValue,maxDrawIndirectCount;
             float maxSamplerLodBias,maxSamplerAnisotropy;
             uint32_t maxViewportDimensions[2]; float viewportBoundsRange[2];
             uint32_t viewportSubPixelBits;
             size_t minMemoryMapAlignment;
             VkDeviceSize minTexelBufferOffsetAlignment,minUniformBufferOffsetAlignment,minStorageBufferOffsetAlignment;
             int32_t minTexelOffset,maxTexelOffset,minTexelGatherOffset,maxTexelGatherOffset;
             float minInterpolationOffset,maxInterpolationOffset;
             uint32_t subPixelInterpolationOffsetBits;
             uint32_t maxFramebufferWidth,maxFramebufferHeight,maxFramebufferLayers;
             VkFlags framebufferColorSampleCounts, framebufferDepthSampleCounts, framebufferStencilSampleCounts, framebufferNoAttachmentsSampleCounts;
             uint32_t maxColorAttachments, sampledImageColorSampleCounts, sampledImageIntegerSampleCounts, sampledImageDepthSampleCounts, sampledImageStencilSampleCounts, storageImageSampleCounts;
             uint32_t maxSampleMaskWords; uint32_t timestampComputeAndGraphics; float timestampPeriod;
             uint32_t maxClipDistances,maxCullDistances,maxCombinedClipAndCullDistances;
             uint32_t discreteQueuePriorities; float pointSizeRange[2]; float lineWidthRange[2]; float pointSizeGranularity,lineWidthGranularity;
             uint32_t strictLines,standardSampleLocations;
             VkDeviceSize optimalBufferCopyOffsetAlignment,optimalBufferCopyRowPitchAlignment;
             VkDeviceSize nonCoherentAtomSize;
    } limits;
    struct { uint32_t residencyStandard2DBlockShape,residencyStandard2DMultisampleBlockShape;
             uint32_t residencyStandard3DBlockShape,residencyAlignedMipSize;
             uint32_t residencyNonResidentStrict; } sparseProperties;
} VkPhysicalDeviceProperties;

/* pNext chains: we only need the sType + pNext + the flags word we read back.
 * Vulkan 1.1/1.2/1.3 feature structs are a chain of {sType,pNext,flags} where
 * flags is a bitfield of VkBool32s. We read the whole struct as an array of
 * VkBool32 words after the header and just report named bits for the ones we
 * care about. Sizes below are the real struct sizes for 64-bit. */
#define VK11_FEATURES_WORDS 80
#define VK12_FEATURES_WORDS 80
#define VK13_FEATURES_WORDS 80

static VkResult (*pCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
static void (*pDestroyInstance)(VkInstance, const void*);
static VkResult (*pEnumerateInstanceExtensionProperties)(const char*, uint32_t*, VkExtensionProperties*);
static VkResult (*pEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
static void (*pGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties*);
static void* (*pGetInstanceProcAddr)(VkInstance, const char*);

/* Feature structs, header + words */
typedef struct { uint32_t sType; const void* pNext; uint32_t words[VK11_FEATURES_WORDS]; } Feat11;
typedef struct { uint32_t sType; const void* pNext; uint32_t words[VK12_FEATURES_WORDS]; } Feat12;
typedef struct { uint32_t sType; const void* pNext; uint32_t words[VK13_FEATURES_WORDS]; } Feat13;

static char g_list[1<<20];
static size_t g_len;

static void get(void *fn, const char *name) { (void)fn; (void)name; }

#define LOAD(dst, name) do { *(void**)&(dst) = (void*)pGetInstanceProcAddr(inst, #name); } while(0)

static void have(const char *const *names, uint32_t n, const char *q) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(names[i], q)) return; /* found */
}

int main(int argc, char **argv) {
    VkExtensions: {
        uint32_t n = 0;
        VkResult r = VK_SUCCESS;
        void *lib = dlopen("libvulkan.so", RTLD_NOW);
        if (!lib) { printf("FATAL dlopen(libvulkan.so): %s\n", dlerror()); return 2; }
        *(void**)&pGetInstanceProcAddr = dlsym(lib, "vkGetInstanceProcAddr");
        if (!pGetInstanceProcAddr) { printf("FATAL no vkGetInstanceProcAddr\n"); return 2; }
        *(void**)&pCreateInstance = (void*)pGetInstanceProcAddr(NULL, "vkCreateInstance");
        *(void**)&pEnumerateInstanceExtensionProperties = (void*)pGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
        *(void**)&pEnumeratePhysicalDevices = (void*)pGetInstanceProcAddr(NULL, "vkEnumeratePhysicalDevices");
        *(void**)&pDestroyInstance = (void*)pGetInstanceProcAddr(NULL, "vkDestroyInstance");
        *(void**)&pGetPhysicalDeviceProperties = (void*)pGetInstanceProcAddr(NULL, "vkGetPhysicalDeviceProperties");
        if (!pCreateInstance || !pEnumerateInstanceExtensionProperties) { printf("FATAL null global procs\n"); return 2; }

        r = pEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        printf("== INSTANCE EXTENSIONS: count=%u result=%d ==\n", n, r);
        if (n) {
            VkExtensionProperties *props = calloc(n, sizeof(*props));
            r = pEnumerateInstanceExtensionProperties(NULL, &n, props);
            for (uint32_t i = 0; i < n; i++) printf("  %s (spec %u)\n", props[i].extensionName, props[i].specVersion);
            free(props);
        }

        VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pvr-vkprobe", 1, "pvr-vkprobe", 1, VK_MAKE_API_VERSION(0,1,1,0) };
        VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
        VkInstance inst = NULL;
        r = pCreateInstance(&ci, NULL, &inst);
        printf("\n== vkCreateInstance(api 1.1) result=%d inst=%p ==\n", r, (void*)inst);
        if (r != VK_SUCCESS || !inst) { printf("instance creation failed\n"); return 1; }

        LOAD(pEnumeratePhysicalDevices, vkEnumeratePhysicalDevices);
        LOAD(pGetPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
        LOAD(pDestroyInstance, vkDestroyInstance);
        LOAD(pGetInstanceProcAddr, vkGetInstanceProcAddr);

        uint32_t dn = 0;
        r = pEnumeratePhysicalDevices(inst, &dn, NULL);
        printf("== PHYSICAL DEVICES: count=%u result=%d ==\n", dn, r);
        VkPhysicalDevice devs[8] = {0};
        if (dn > 8) dn = 8;
        r = pEnumeratePhysicalDevices(inst, &dn, devs);
        for (uint32_t i = 0; i < dn; i++) {
            VkPhysicalDeviceProperties p;
            memset(&p, 0, sizeof(p));
            pGetPhysicalDeviceProperties(devs[i], &p);

            /* Device extensions */
            uint32_t en = 0;
            VkResult (*pEd)(VkPhysicalDevice, const char*, uint32_t*, VkExtensionProperties*) =
                (void*)pGetInstanceProcAddr(inst, "vkEnumerateDeviceExtensionProperties");
            printf("\n---- device[%u] %s ----\n", i, p.deviceName);
            printf("apiVersion=%u.%u.%u driverVersion=%u vendorID=0x%X deviceID=0x%X deviceType=%u\n",
                   VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                   p.apiVersion & 0xFFF, p.driverVersion, p.vendorID, p.deviceID, p.deviceType);

            if (pEd) {
                r = pEd(devs[i], NULL, &en, NULL);
                printf("device extensions: count=%u result=%d\n", en, r);
                if (en) {
                    VkExtensionProperties *ep = calloc(en, sizeof(*ep));
                    r = pEd(devs[i], NULL, &en, ep);
                    /* Print the DRM/dma-buf/external-memory/sync set in full, rest summarized */
                    for (uint32_t k = 0; k < en; k++) {
                        const char *s = ep[k].extensionName;
                        if (strstr(s,"drm")||strstr(s,"dma")||strstr(s,"external")||strstr(s,"android")||
                            strstr(s,"timeline")||strstr(s,"sync")||strstr(s,"swapchain")||strstr(s,"format")||
                            strstr(s,"memory")||strstr(s,"image")||strstr(s,"robust")||strstr(s,"render")||
                            strstr(s,"descriptor")||strstr(s,"multiview")||strstr(s,"depth")||strstr(s,"imageless")||
                            strstr(s,"separate")||strstr(s,"sampler")||strstr(s,"maintenance")||strstr(s,"pipeline")||
                            strstr(s,"shader")||strstr(s,"variable")||strstr(s,"draw")||strstr(s,"fragment"))
                            printf("  EXT %s (spec %u)\n", s, ep[k].specVersion);
                    }
                    printf("  [full list follows]\n");
                    for (uint32_t k = 0; k < en; k++) printf("  ALL %s\n", ep[k].extensionName);
                    free(ep);
                }
            }

            /* Features: query VkPhysicalDeviceFeatures2 (1.0 core) + 1.1/1.2/1.3 */
            typedef void (*PFN_gf2)(VkPhysicalDevice, void*);
            PFN_gf2 gf2 = (void*)pGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2");
            if (!gf2) gf2 = (void*)pGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2KHR");
            printf("vkGetPhysicalDeviceFeatures2 = %s\n", gf2 ? "present" : "MISSING");
            if (gf2 && VK_API_VERSION_MINOR(p.apiVersion) >= 1) {
                Feat13 f13; memset(&f13, 0, sizeof(f13)); f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                Feat12 f12; memset(&f12, 0, sizeof(f12)); f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; f12.pNext = &f13;
                Feat11 f11; memset(&f11, 0, sizeof(f11)); f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES; f11.pNext = &f12;
                struct { uint32_t sType; const void* pNext; uint32_t words[128]; } f10;
                memset(&f10, 0, sizeof(f10)); f10.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; f10.pNext = &f11;
                gf2(devs[i], &f10);
                printf("features(1.0 core) words[0..3]=%08x %08x %08x %08x\n",
                       f10.words[0],f10.words[1],f10.words[2],f10.words[3]);
                printf("features(1.1) words[0..3]=%08x %08x %08x %08x\n",
                       f11.words[0],f11.words[1],f11.words[2],f11.words[3]);
                printf("features(1.2) words[0..3]=%08x %08x %08x %08x\n",
                       f12.words[0],f12.words[1],f12.words[2],f12.words[3]);
                printf("features(1.3) words[0..3]=%08x %08x %08x %08x\n",
                       f13.words[0],f13.words[1],f13.words[2],f13.words[3]);
            }

        }
        pDestroyInstance(inst, NULL);
        printf("\n== DONE ==\n");
    }
    (void)g_list; (void)g_len; (void)get; (void)have;
    return 0;
}
