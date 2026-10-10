/* pvr_hybris_test.c — load the bionic PowerVR HAL through libhybris, in a glibc process.
 *
 * Not plain dlopen: the blob and its deps are bionic, so they must be loaded by libhybris's
 * emulated bionic linker (mm.so, reached via android_dlopen_ext) with the libhybris libc hooks
 * translating bionic -> glibc. If this returns HMI and drives vkCreateInstance, the shim path
 * is real. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    void *bt[48];
    int n = backtrace(bt, 48);
    char buf[128];
    int k = snprintf(buf, sizeof(buf), "\n*** SIGSEGV addr=%p ***\n", si->si_addr);
    write(2, buf, k);
    backtrace_symbols_fd(bt, n, 2);
    _exit(139);
}

/* libhybris entry points */
extern void *android_dlopen_ext(const char *filename, int flag, const void *info);
extern void *android_dlsym(void *handle, const char *symbol);
extern char *android_dlerror(void);

typedef int32_t VkResult;
typedef struct VkInstance_T *VkInstance;
typedef struct { uint32_t sType; const void *pNext; const char *pApp; uint32_t appVer; const char *pEngine; uint32_t engVer; uint32_t api; } VkApplicationInfo;
typedef struct { uint32_t sType; const void *pNext; uint32_t flags; const VkApplicationInfo *pApp; uint32_t nLayers; const char *const *pLayers; uint32_t nExt; const char *const *pExt; } VkInstanceCreateInfo;

#define VK_SUCCESS 0
#define VKMK(v,va,ma,mi) ((((uint32_t)(v))<<29)|(((uint32_t)(va))<<22)|(((uint32_t)(ma))<<12)|((uint32_t)(mi)))

typedef struct hw_module_t hw_module_t;
typedef struct { int (*open)(const hw_module_t *, const char *, void **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id, *name, *author; hw_module_methods_t *methods; void *dso; uint32_t reserved[25];
};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    printf("== libhybris + PowerVR HAL test ==\n");
    printf("HYBRIS_LINKER_DIR=%s HYBRIS_LD_LIBRARY_PATH=%s\n",
           getenv("HYBRIS_LINKER_DIR"), getenv("HYBRIS_LD_LIBRARY_PATH"));

    printf("android_dlopen_ext(%s) ...\n", "/vendor/lib64/hw/vulkan.powervr.so");
    void *h = android_dlopen_ext("/vendor/lib64/hw/vulkan.powervr.so", 2 /*RTLD_NOW*/, NULL);
    if (!h) { printf("  FAIL: %s\n", android_dlerror()); return 2; }
    printf("  OK handle=%p\n", h);

    hw_module_t *mod = (hw_module_t *)android_dlsym(h, "HMI");
    if (!mod) { printf("  no HMI: %s\n", android_dlerror()); return 2; }
    printf("HMI: name=%s author=%s methods=%p\n", mod->name ? mod->name : "?", mod->author ? mod->author : "?", (void *)mod->methods);
    if (!mod->methods || !mod->methods->open) { printf("  no open()\n"); return 2; }

    void *dev = NULL;
    int rc = mod->methods->open(mod, "vk0", &dev);
    printf("open(vk0) rc=%d dev=%p\n", rc, dev);
    if (rc != 0 || !dev) return 2;
    uint64_t *w = (uint64_t *)dev;
    void *(*gipa)(VkInstance, const char *) = (void *)w[0x88 / 8];
    VkResult (*createInstance)(const void *, const void *, VkInstance *) = (void *)w[0x80 / 8];
    printf("GIPA=%p CreateInstance=%p\n", (void *)gipa, (void *)createInstance);
    if (!gipa) return 2;
    if (!createInstance) createInstance = (void *)gipa(NULL, "vkCreateInstance");

    VkApplicationInfo ai; memset(&ai, 0, sizeof(ai));
    ai.sType = 0; ai.pApp = "pvr-hybris"; ai.appVer = 1; ai.pEngine = "pvr-hybris"; ai.engVer = 1; ai.api = VKMK(0, 1, 1, 0);
    VkInstanceCreateInfo ci; memset(&ci, 0, sizeof(ci)); ci.sType = 1; ci.pApp = &ai;
    VkInstance inst = NULL;
    VkResult r = createInstance(&ci, NULL, &inst);
    printf("vkCreateInstance -> %d inst=%p\n", r, (void *)inst);
    if (r != VK_SUCCESS || !inst) { printf("RESULT: HAL loaded under glibc, but instance failed\n"); return 1; }

    void (*getProps)(void *, void *) = (void *)gipa(inst, "vkGetPhysicalDeviceProperties");
    VkResult (*enumPhys)(VkInstance, uint32_t *, void **) = (void *)gipa(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 0; enumPhys(inst, &n, NULL); void *pd = NULL; enumPhys(inst, &n, &pd);
    struct { uint32_t api, drv, vend, dev, type; char name[256]; } p; memset(&p, 0, sizeof(p));
    getProps(pd, &p);
    printf("RESULT: SUCCESS - %u device(s): \"%s\" api=%u.%u.%u vendor=0x%x\n", n, p.name,
           (p.api >> 22) & 0x7f, (p.api >> 12) & 0x3ff, p.api & 0xfff, p.vend);

    /* Prove the DXVK-critical capability set is reachable through the glibc path too. */
    VkResult (*enumDevExt)(void *, const char *, uint32_t *, void *) = (void *)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    void (*getFmtProps)(void *, int, uint32_t *) = (void *)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    if (enumDevExt) {
        uint32_t en = 0; enumDevExt(pd, NULL, &en, NULL);
        printf("device extensions visible through glibc: %u\n", en);
        struct { char name[256]; uint32_t v; } *eps = calloc(en ? en : 1, sizeof(*eps));
        enumDevExt(pd, NULL, &en, eps);
        const char *want[] = { "VK_EXT_image_drm_format_modifier", "VK_EXT_external_memory_dma_buf",
                               "VK_KHR_external_memory_fd", "VK_KHR_timeline_semaphore", "VK_KHR_dynamic_rendering" };
        for (unsigned i = 0; i < 5; i++) { int f = 0; for (uint32_t k = 0; k < en; k++) if (!strcmp(eps[k].name, want[i])) f = 1;
            printf("  %-40s %s\n", want[i], f ? "present" : "MISSING"); }
        free(eps);
    }
    if (getFmtProps) {
        uint32_t fp[3] = {0, 0, 0};
        getFmtProps(pd, 137 /*BC3_UNORM*/, fp);  /* optimalTilingFeatures is fp[1] */
        printf("BC3_UNORM(137) optimalTilingFeatures=0x%08x  %s\n", fp[1], (fp[1] & 0x1u) ? "SAMPLED (DXVK DXTn OK)" : "not sampled");
    }
    /* Exit without teardown: bionic's stack-canary guard differs from glibc's, so unwinding the
     * bionic frames at process exit can trip __stack_chk_fail. The point of the probe is the load. */
    printf("RESULT: the bionic PowerVR HAL drove a Vulkan device from a glibc process.\n");
    fflush(NULL);
    _exit(0);
}
