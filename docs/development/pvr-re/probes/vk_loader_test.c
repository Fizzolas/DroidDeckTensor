/* vk_loader_test.c — drive the *glibc Vulkan loader* against the PowerVR ICD.
 *
 * The full guest chain, in one glibc process:
 *   this program -> libvulkan.so.1 (glibc loader) -> VK_ICD_FILENAMES manifest
 *   -> libvulkan_powervr.so (our ICD) -> libhybris -> bionic vulkan.powervr.so HAL -> GPU
 *
 * Env: VK_ICD_FILENAMES=<manifest>, HYBRIS_LINKER_DIR, HYBRIS_LD_LIBRARY_PATH.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <execinfo.h>
#include <dlfcn.h>

#include <ucontext.h>
/* Print the interrupted PC and which DSO mapping it lands in (no unwinding: safe here). */
static void on_alarm(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)si;
    ucontext_t *u = (ucontext_t *)uc;
    uintptr_t pc = (uintptr_t)u->uc_mcontext.pc;
    uintptr_t lr = (uintptr_t)u->uc_mcontext.regs[30], sp = (uintptr_t)u->uc_mcontext.sp;
    char b[512];
    int n = snprintf(b, sizeof(b), "\n*** HANG main: pc=%p lr=%p sp=%p\n", (void *)pc, (void *)lr, (void *)sp);
    write(2, b, n);
    /* find the mapping containing pc */
    FILE *m = fopen("/proc/self/maps", "r");
    if (m) {
        char line[512];
        while (fgets(line, sizeof(line), m)) {
            unsigned long a, z;
            if (sscanf(line, "%lx-%lx", &a, &z) == 2 && pc >= a && pc < z) {
                n = snprintf(b, sizeof(b), "*** PC in: %s", line); write(2, b, n); break;
            }
        }
        fclose(m);
    }
    _exit(124);
}

typedef int32_t VkResult;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct { uint32_t sType; const void *pNext; const char *pApp; uint32_t appVer; const char *pEngine; uint32_t engVer; uint32_t api; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const VkApplicationInfo *pApp; uint32_t nLayers; const char *const *pLayers; uint32_t nExt; const char *const *pExt; } VkInstanceCreateInfo;
#define VKMK(v,va,ma,mi) ((((uint32_t)(v))<<29)|(((uint32_t)(va))<<22)|(((uint32_t)(ma))<<12)|((uint32_t)(mi)))

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== glibc Vulkan loader -> PowerVR ICD test ==\n");
    printf("VK_ICD_FILENAMES=%s\n", getenv("VK_ICD_FILENAMES"));

    void *vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk) { printf("dlopen(libvulkan.so.1) failed: %s\n", dlerror()); return 2; }
    void *(*gipa)(VkInstance, const char *) = dlsym(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr in the loader\n"); return 2; }

    VkResult (*createInstance)(const void *, const void *, VkInstance *) = (void *)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.sType = 0; ai.pApp = "vk-loader-test"; ai.appVer = 1; ai.pEngine = "none"; ai.engVer = 1; ai.api = VKMK(0, 1, 1, 0);
    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof(ci)); ci.sType = 1; ci.pApp = &ai;
    VkInstance inst = NULL;
    VkResult r = createInstance(&ci, NULL, &inst);
    printf("vkCreateInstance -> %d inst=%p\n", r, (void *)inst);
    if (r != 0 || !inst) { printf("RESULT: loader could not create an instance\n"); return 1; }

    VkResult (*enumPhys)(VkInstance, uint32_t *, VkPhysicalDevice *) = (void *)gipa(inst, "vkEnumeratePhysicalDevices");
    void (*getProps)(VkPhysicalDevice, void *) = (void *)gipa(inst, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0; enumPhys(inst, &n, NULL);
    printf("physical devices: %u\n", n);
    if (n == 0) { printf("RESULT: loader found 0 devices\n"); return 1; }
    VkPhysicalDevice pd = NULL; enumPhys(inst, &n, &pd);
    struct { uint32_t api, drv, vend, dev, type; char name[256]; } p; memset(&p, 0, sizeof(p));
    getProps(pd, &p);
    printf("RESULT: SUCCESS - \"%s\" api=%u.%u.%u vendor=0x%x device=0x%x\n", p.name,
           (p.api >> 22) & 0x7f, (p.api >> 12) & 0x3ff, p.api & 0xfff, p.vend, p.dev);

    /* Device creation: the link the guest actually needs to render. */
    void (*getQFam)(VkPhysicalDevice, uint32_t *, void *) = (void *)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t nq = 0; getQFam(pd, &nq, NULL);
    printf("queue families: %u\n", nq);
    if (nq) {
        struct { uint32_t flags, count, ts; } *qf = calloc(nq, sizeof(*qf));
        getQFam(pd, &nq, qf);
        int graphics = -1;
        for (uint32_t i = 0; i < nq; i++) {
            printf("  family %u: flags=0x%x queues=%u%s\n", i, qf[i].flags, qf[i].count, (qf[i].flags & 1) ? " (GRAPHICS)" : "");
            if ((qf[i].flags & 1) && graphics < 0) graphics = (int)i;
        }
        if (graphics >= 0) {
            float prio = 1.0f;
            /* VkDeviceQueueCreateInfo: sType=14, pNext, flags, queueFamilyIndex, queueCount, pQueuePriorities */
            struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t family; uint32_t count; const float *prios; } qci;
            memset(&qci, 0, sizeof(qci));
            qci.sType = 14; qci.family = (uint32_t)graphics; qci.count = 1; qci.prios = &prio;
            /* VkDeviceCreateInfo: sType=3, pNext, flags, qfc, pQci, nLayers, pLayers, nExt, pExt, pEnabledFeatures */
            struct { uint32_t sType; const void *pNext; uint32_t flags; uint32_t qfc; const void *pqci;
                     uint32_t nLayers; const void *pLayers; uint32_t nExt; const void *pExt; const void *pEnabled; } dci;
            memset(&dci, 0, sizeof(dci)); dci.sType = 3; dci.qfc = 1; dci.pqci = &qci;
            VkResult (*createDevice)(VkPhysicalDevice, const void *, const void *, void **) = (void *)gipa(inst, "vkCreateDevice");
            void *dev = NULL;
            struct sigaction sa; memset(&sa, 0, sizeof(sa));
            sa.sa_sigaction = on_alarm; sa.sa_flags = SA_SIGINFO;
            sigaction(SIGALRM, &sa, NULL); alarm(25);
            VkResult dr = createDevice ? createDevice(pd, &dci, NULL, &dev) : -1;
            alarm(0);
            printf("vkCreateDevice(graphics family %d) -> %d device=%p\n", graphics, dr, dev);
        }
        free(qf);
    }
    printf("RESULT: the glibc Vulkan loader enumerated the PowerVR GPU through the shim ICD.\n");
    fflush(NULL);
    _exit(0);
}
