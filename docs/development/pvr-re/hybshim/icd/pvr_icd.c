#define _GNU_SOURCE 1
/*
 * pvr_icd.c — a glibc Vulkan ICD that drives the bionic PowerVR Vulkan HAL through libhybris.
 *
 * Built as `libvulkan_powervr.so`; a glibc Vulkan loader discovers it by a manifest whose
 * library_path points here (VK_ICD_FILENAMES / VK_DRIVER_FILES). It:
 *   1. is linked against libhybris.so (DT_NEEDED), so the bionic->glibc hook layer is present;
 *   2. uses libhybris's android_dlopen_ext to load the bionic HAL and its deps;
 *   3. gets HMI (hw_module_t), open("vk0") -> the hwvulkan device, and its GetInstanceProcAddr;
 *   4. forwards the loader's ICD entrypoints straight to the HAL.
 *
 * The HAL is a full Vulkan implementation (1.4.317 on Tensor G5), so this wrapper is thin: it
 * exists only to load the bionic code in a glibc process and re-expose it under the ICD ABI.
 *
 * Env the process must set:
 *   HYBRIS_LINKER_DIR      = dir holding the libhybris linker DSO (q.so)
 *   HYBRIS_LD_LIBRARY_PATH = /system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <ucontext.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/mman.h>

/* libhybris entry points (provided by libhybris.so, which this DSO links against). */
extern void *android_dlopen_ext(const char *filename, int flag, const void *info);
extern void *android_dlsym(void *handle, const char *symbol);
extern char *android_dlerror(void);

#ifndef RTLD_NOW
#define RTLD_NOW 2   /* bionic's value; the host includes often define their own */
#endif
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY (-2)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_MEMORY_MAP_FAILED (-5)
#define VK_INCOMPLETE 5
#define VK_ERROR_LAYER_NOT_PRESENT (-6)
#define VK_ERROR_UNKNOWN (-13)
typedef int32_t VkResult;

typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;

/* Android hw_module_t / hwvulkan device, spelled out (no Android headers). */
typedef struct hw_module_t hw_module_t;
typedef struct { int (*open)(const hw_module_t *, const char *, void **); } hw_module_methods_t;
struct hw_module_t {
    uint32_t tag; uint16_t module_api_version; uint16_t hal_api_version;
    const char *id, *name, *author; hw_module_methods_t *methods; void *dso; uint32_t reserved[25];
};
typedef struct { uint64_t w[18]; } hwvulkan_device_t; /* CreateInstance @0x80, GIPA @0x88 (from the blob's open()) */

static const char *const BLOB = "/vendor/lib64/hw/vulkan.powervr.so";

static void *g_blob;                 /* the HAL module handle */
static void *(*g_gipa)(VkInstance, const char *);   /* the HAL's vkGetInstanceProcAddr */
static void *(*g_create)(const void *, const void *, VkInstance *);
static VkInstance g_instance;         /* the instance we made for the HAL */

/* The instance extensions this ICD provides on top of the HAL (the HAL has only Android surfaces). */
static const char *const ICD_INSTANCE_EXTENSIONS[] = {
    "VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
    "VK_KHR_get_surface_capabilities2" };
#define ICD_NUM_INSTANCE_EXTENSIONS 5

static void icd_log(const char *m) { fprintf(stderr, "pvr_icd: %s\n", m); }

/* Per-call tracing, off unless asked for. gamescope probes a DRM modifier once per output format per
 * frame, so an unconditional line here buried the session log under hundreds of thousands of them and
 * made the guest's own errors hard to find. The fixes themselves stay silent: a retry that succeeds is
 * the expected path now, not news. `PVR_ICD_DEBUG=1` in the guest's environment turns it back on. */
static int icd_debug_on(void) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("PVR_ICD_DEBUG"); on = (v && *v && *v != '0'); }
    return on;
}

/* The Wayland WSI this ICD carries (the HAL only does Android surfaces). */
extern void *pvr_wsi_proc(const char *name);
extern void *pvr_wsi_sync_proc(const char *name);
/* Told about each device the HAL creates, so the WSI can serialize HAL queue submits and complete
 * acquire fences/semaphores with the HAL's own queue entries (see the note in pvr_wsi.c). */
extern void pvr_wsi_register_device(VkDevice dev, uint32_t queue_family, void *queue_submit,
                                    void *create_fence, void *wait_fences, void *destroy_fence,
                                    void *get_device_queue, void *queue_submit2,
                                    void *queue_submit2_khr, void *queue_wait_idle,
                                    void *device_wait_idle, void *get_device_queue2,
                                    void *destroy_device);
static int icd_load(void);   /* defined below; used by pvr_icd_resolve */
static VkResult icd_create_image(void *device, const void *ci, const void *alloc, void **out);  /* fwd */
static VkResult icd_create_shader_module(void *device, const void *ci, const void *alloc, void **out); /* fwd */
static VkResult icd_create_compute_pipelines(void *device, void *pipelineCache, uint32_t count,
                                             const void *pCreateInfos, const void *alloc, void **pPipelines); /* fwd */
static VkResult icd_create_graphics_pipelines(void *device, void *pipelineCache, uint32_t count,
                                              const void *pCreateInfos, const void *alloc, void **pPipelines); /* fwd */
static void icd_get_phys_dev_props2(void *pd, void *props2); /* fwd */
static VkResult icd_get_phys_dev_img_fmt_props2(void *pd, void *info, void *out); /* fwd */
static void *icd_gpdpa(void *pd, const char *pName); /* fwd */

/* Self-configuration: the guest session runs with a clean environment (env -i), so this ICD points
 * the libhybris layer at its own directory (found from this object's path) if the caller has not. */
/* Which process is this? Cmdline, not comm: Steam's processes are all "steam"/"steamwebhelper" in
 * comm, and the question this answers is exactly which one loads this ICD and which one presents. */
const char *pvr_proc_cmd(void) {
    static char cmd[256];
    if (!cmd[0]) {
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, cmd, sizeof cmd - 1);
            if (n > 0) { for (ssize_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' '; cmd[n] = 0; }
            else cmd[0] = '?', cmd[1] = 0;
            close(fd);
        } else { strcpy(cmd, "?"); }
    }
    return cmd;
}

static const char *pvr_proc_comm(void) {
    static char comm[64];
    if (!comm[0]) {
        int fd = open("/proc/self/comm", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, comm, sizeof(comm) - 1);
            if (n > 0) {
                while (n > 0 && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) n--;
                comm[n] = 0;
            }
            close(fd);
        }
        if (!comm[0]) strcpy(comm, "?");
    }
    return comm;
}

static void icd_wsi_write_all(const char *buf, size_t len) {
    if (!buf || !len) return;
    int fd = open("/tmp/pvr_wsi.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            break;
        }
        off += (size_t)w;
    }
    close(fd);
}

static void icd_wsi_logf(const char *fmt, ...) {
    char buf[1024];
    long tid = (long)syscall(SYS_gettid);
    int pfx = snprintf(buf, sizeof(buf), "pvr_icd pid=%d tid=%ld comm=%s: ",
                       (int)getpid(), tid, pvr_proc_comm());
    if (pfx < 0) return;
    if ((size_t)pfx >= sizeof(buf)) pfx = (int)sizeof(buf) - 1;
    va_list ap;
    va_start(ap, fmt);
    int body = vsnprintf(buf + pfx, sizeof(buf) - (size_t)pfx, fmt, ap);
    va_end(ap);
    int total = pfx + (body > 0 ? body : 0);
    if ((size_t)total >= sizeof(buf)) total = (int)sizeof(buf) - 1;
    if (total == 0 || buf[total - 1] != '\n') {
        if ((size_t)total < sizeof(buf) - 1) buf[total++] = '\n';
        else buf[total - 1] = '\n';
    }
    buf[total] = 0;
    icd_wsi_write_all(buf, (size_t)total);
}

/* ---- Async-signal-safe backtrace + fault + X11 error capture ---- */

typedef struct {
    char buf[4096];
    size_t len;
} sig_buf_t;

static void sb_flush(sig_buf_t *sb, int also_stderr) {
    if (!sb->len) return;
    icd_wsi_write_all(sb->buf, sb->len);
    if (also_stderr) {
        size_t off = 0;
        while (off < sb->len) {
            ssize_t w = write(2, sb->buf + off, sb->len - off);
            if (w <= 0) {
                if (w < 0 && errno == EINTR) continue;
                break;
            }
            off += (size_t)w;
        }
    }
    sb->len = 0;
}

static void sb_putc(sig_buf_t *sb, char c, int also_stderr) {
    if (sb->len >= sizeof(sb->buf)) sb_flush(sb, also_stderr);
    sb->buf[sb->len++] = c;
}

static void sb_str(sig_buf_t *sb, const char *s, int also_stderr) {
    if (!s) s = "(null)";
    for (size_t i = 0; s[i]; i++) sb_putc(sb, s[i], also_stderr);
}

static void sb_hex(sig_buf_t *sb, unsigned long long v, int also_stderr) {
    sb_str(sb, "0x", also_stderr);
    if (v == 0) { sb_putc(sb, '0', also_stderr); return; }
    char tmp[20];
    int n = 0;
    while (v) {
        unsigned d = (unsigned)(v & 0xfU);
        tmp[n++] = (char)(d < 10 ? ('0' + d) : ('a' + (d - 10)));
        v >>= 4;
    }
    while (n > 0) sb_putc(sb, tmp[--n], also_stderr);
}

static void sb_dec(sig_buf_t *sb, long long v, int also_stderr) {
    if (v < 0) {
        sb_putc(sb, '-', also_stderr);
        unsigned long long uv = (unsigned long long)(-(v + 1)) + 1ULL;
        char tmp[24];
        int n = 0;
        while (uv) { tmp[n++] = (char)('0' + (uv % 10ULL)); uv /= 10ULL; }
        while (n > 0) sb_putc(sb, tmp[--n], also_stderr);
        return;
    }
    if (v == 0) { sb_putc(sb, '0', also_stderr); return; }
    unsigned long long uv = (unsigned long long)v;
    char tmp[24];
    int n = 0;
    while (uv) { tmp[n++] = (char)('0' + (uv % 10ULL)); uv /= 10ULL; }
    while (n > 0) sb_putc(sb, tmp[--n], also_stderr);
}

static int sig_parse_hex(const char **pp, uintptr_t *out) {
    const char *p = *pp;
    uintptr_t v = 0;
    int any = 0;
    while (*p) {
        char c = *p;
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
        any = 1;
        p++;
    }
    *pp = p;
    *out = v;
    return any;
}

typedef struct {
    uintptr_t addr;
    uintptr_t lo;
    uintptr_t off;
    const char *tag;
    char path[64];
    int matched;
} sig_addr_slot_t;

#define SIG_MAX_SLOTS 28

static void sig_parse_maps_line(const char *line, uintptr_t *lo, uintptr_t *hi,
                                uintptr_t *off, char perms[4], const char **path_out) {
    const char *p = line;
    *lo = *hi = *off = 0;
    perms[0] = perms[1] = perms[2] = perms[3] = 0;
    *path_out = "";
    if (!sig_parse_hex(&p, lo) || *p != '-') return;
    p++;
    if (!sig_parse_hex(&p, hi) || *p != ' ') return;
    while (*p == ' ') p++;
    for (int i = 0; i < 4 && *p && *p != ' '; i++) perms[i] = *p++;
    while (*p == ' ') p++;
    sig_parse_hex(&p, off);
    /* skip dev and inode fields */
    for (int col = 0; col < 2; col++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    while (*p == ' ') p++;
    *path_out = p;
}

static void sig_scan_maps_for_stack(uintptr_t sp, uintptr_t *slo, uintptr_t *shi) {
    *slo = *shi = 0;
    if (!sp) return;
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    char buf[1024];
    size_t carry = 0;
    for (;;) {
        ssize_t n = read(fd, buf + carry, sizeof(buf) - 1 - carry);
        if (n <= 0) break;
        size_t total = carry + (size_t)n;
        buf[total] = 0;
        size_t start = 0;
        for (size_t i = 0; i < total; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                uintptr_t lo, hi, off;
                char perms[4];
                const char *path;
                sig_parse_maps_line(buf + start, &lo, &hi, &off, perms, &path);
                if (lo <= sp && sp < hi && perms[0] == 'r') {
                    *slo = lo;
                    *shi = hi;
                    close(fd);
                    return;
                }
                start = i + 1;
            }
        }
        carry = total - start;
        if (carry >= sizeof(buf) - 2) carry = 0;
        else if (carry > 0) memmove(buf, buf + start, carry);
    }
    close(fd);
}

static void sig_resolve_slots(sig_addr_slot_t *slots, int nslots,
                              const uintptr_t *stk_cands, int nstk,
                              uintptr_t *stk_lo, uintptr_t *stk_off,
                              char (*stk_path)[48], int *stk_hit) {
    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;
    char buf[1024];
    size_t carry = 0;
    for (;;) {
        ssize_t n = read(fd, buf + carry, sizeof(buf) - 1 - carry);
        if (n <= 0) break;
        size_t total = carry + (size_t)n;
        buf[total] = 0;
        size_t start = 0;
        for (size_t i = 0; i < total; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                uintptr_t lo, hi, off;
                char perms[4];
                const char *path;
                sig_parse_maps_line(buf + start, &lo, &hi, &off, perms, &path);
                if (hi > lo) {
                    for (int k = 0; k < nslots; k++) {
                        if (!slots[k].matched && slots[k].addr >= lo && slots[k].addr < hi) {
                            slots[k].matched = 1;
                            slots[k].lo = lo;
                            slots[k].off = off;
                            size_t j = 0;
                            while (j < sizeof(slots[k].path) - 1 && path[j] && path[j] != '\n') {
                                slots[k].path[j] = path[j];
                                j++;
                            }
                            slots[k].path[j] = 0;
                        }
                    }
                    if (perms[2] == 'x' && path[0] == '/') {
                        for (int k = 0; k < nstk; k++) {
                            if (!stk_hit[k] && stk_cands[k] >= lo && stk_cands[k] < hi) {
                                stk_hit[k] = 1;
                                stk_lo[k] = lo;
                                stk_off[k] = off;
                                size_t j = 0;
                                while (j < 47 && path[j] && path[j] != '\n') {
                                    stk_path[k][j] = path[j];
                                    j++;
                                }
                                stk_path[k][j] = 0;
                            }
                        }
                    }
                }
                start = i + 1;
            }
        }
        carry = total - start;
        if (carry >= sizeof(buf) - 2) carry = 0;
        else if (carry > 0) memmove(buf, buf + start, carry);
    }
    close(fd);
}

static void sig_dump_backtrace(sig_buf_t *sb, uintptr_t pc, uintptr_t lr,
                               uintptr_t sp, uintptr_t fp, uintptr_t extra_addr,
                               const char *extra_tag, int also_stderr) {
    sig_addr_slot_t slots[SIG_MAX_SLOTS];
    int nslots = 0;
    uintptr_t slo = 0, shi = 0;
    uintptr_t walk_sp = sp ? sp : fp;
    if (!walk_sp) {
        uintptr_t cur_sp = 0;
        __asm__ volatile("mov %0, sp" : "=r"(cur_sp));
        walk_sp = cur_sp;
    }
    sig_scan_maps_for_stack(walk_sp, &slo, &shi);

    if (pc && nslots < SIG_MAX_SLOTS) {
        slots[nslots].addr = pc; slots[nslots].tag = "pc"; slots[nslots].matched = 0;
        slots[nslots].path[0] = 0; nslots++;
    }
    if (lr && nslots < SIG_MAX_SLOTS) {
        slots[nslots].addr = lr; slots[nslots].tag = "lr"; slots[nslots].matched = 0;
        slots[nslots].path[0] = 0; nslots++;
    }
    if (extra_addr && nslots < SIG_MAX_SLOTS) {
        slots[nslots].addr = extra_addr; slots[nslots].tag = extra_tag ? extra_tag : "extra";
        slots[nslots].matched = 0; slots[nslots].path[0] = 0; nslots++;
    }

    int nfp = 0;
    uintptr_t cur = fp;
    for (int i = 0; i < 18 && cur >= slo && cur + 16 <= shi && (cur & 7) == 0 && nslots < SIG_MAX_SLOTS; i++) {
        const uintptr_t *fr = (const uintptr_t *)cur;
        uintptr_t next_fp = fr[0];
        uintptr_t ret_lr  = fr[1];
        if (ret_lr > 0x10000ULL) {
            slots[nslots].addr = ret_lr;
            slots[nslots].tag = "fp";
            slots[nslots].matched = 0;
            slots[nslots].path[0] = 0;
            nslots++;
            nfp++;
        }
        if (next_fp <= cur || next_fp >= shi) break;
        cur = next_fp;
    }

    uintptr_t stk_cands[64];
    uintptr_t stk_lo[64], stk_off[64];
    char stk_path[64][48];
    int stk_hit[64];
    int nstk = 0;
    if (nfp < 6 && walk_sp >= slo && walk_sp + 16 <= shi && (walk_sp & 7) == 0) {
        uintptr_t end_sp = walk_sp + 1024;
        if (end_sp > shi) end_sp = shi;
        for (uintptr_t p = walk_sp; p + 8 <= end_sp && nstk < 64; p += 8) {
            uintptr_t v = *(const uintptr_t *)p;
            if (v > 0x100000ULL && (v & 3) == 0) {
                stk_cands[nstk] = v;
                stk_hit[nstk] = 0;
                stk_lo[nstk] = 0;
                stk_off[nstk] = 0;
                stk_path[nstk][0] = 0;
                nstk++;
            }
        }
    }

    sig_resolve_slots(slots, nslots, stk_cands, nstk, stk_lo, stk_off, stk_path, stk_hit);

    for (int i = 0; i < nslots; i++) {
        sb_str(sb, "  [", also_stderr);
        sb_str(sb, slots[i].tag, also_stderr);
        sb_str(sb, "] ", also_stderr);
        sb_hex(sb, (unsigned long long)slots[i].addr, also_stderr);
        if (slots[i].matched) {
            sb_str(sb, " ", also_stderr);
            sb_str(sb, slots[i].path[0] ? slots[i].path : "(anon)", also_stderr);
            sb_str(sb, " (segoff=", also_stderr);
            sb_hex(sb, (unsigned long long)(slots[i].addr - slots[i].lo), also_stderr);
            sb_str(sb, " fileoff=", also_stderr);
            sb_hex(sb, (unsigned long long)(slots[i].addr - slots[i].lo + slots[i].off), also_stderr);
            sb_putc(sb, ')', also_stderr);
        }
        sb_putc(sb, '\n', also_stderr);
    }
    if (nstk > 0) {
        int printed = 0;
        for (int i = 0; i < nstk && printed < 16; i++) {
            if (!stk_hit[i]) continue;
            sb_str(sb, "  [stk] ", also_stderr);
            sb_hex(sb, (unsigned long long)stk_cands[i], also_stderr);
            sb_str(sb, " ", also_stderr);
            sb_str(sb, stk_path[i], also_stderr);
            sb_str(sb, " (segoff=", also_stderr);
            sb_hex(sb, (unsigned long long)(stk_cands[i] - stk_lo[i]), also_stderr);
            sb_str(sb, " fileoff=", also_stderr);
            sb_hex(sb, (unsigned long long)(stk_cands[i] - stk_lo[i] + stk_off[i]), also_stderr);
            sb_str(sb, ")\n", also_stderr);
            printed++;
        }
    }
}

static const int ICD_CAUGHT_SIGS[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP };
#define ICD_NUM_CAUGHT_SIGS 6
static struct sigaction g_prev_sa[64];
static unsigned char g_prev_sa_valid[64];
static volatile int g_sig_log_count;
static pthread_mutex_t g_sig_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread volatile int t_in_sig = 0;

static void icd_sig_handler(int sig, siginfo_t *si, void *uctx) {
    if (t_in_sig) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, NULL);
        raise(sig);
        return;
    }
    t_in_sig = 1;

    int count = __sync_fetch_and_add(&g_sig_log_count, 1);
    if (count < 200) {
        ucontext_t *uc = (ucontext_t *)uctx;
        uintptr_t pc = uc ? (uintptr_t)uc->uc_mcontext.pc : 0;
        uintptr_t sp = uc ? (uintptr_t)uc->uc_mcontext.sp : 0;
        uintptr_t fp = uc ? (uintptr_t)uc->uc_mcontext.regs[29] : 0;
        uintptr_t lr = uc ? (uintptr_t)uc->uc_mcontext.regs[30] : 0;
        uintptr_t fault_addr = si ? (uintptr_t)si->si_addr : 0;

        sig_buf_t sb;
        sb.len = 0;
        sb_str(&sb, "pvr_icd SIGNAL sig=", 1);
        sb_dec(&sb, sig, 1);
        sb_str(&sb, " pid=", 1);
        sb_dec(&sb, (long long)getpid(), 1);
        sb_str(&sb, " tid=", 1);
        sb_dec(&sb, (long long)syscall(SYS_gettid), 1);
        sb_str(&sb, " comm=", 1);
        sb_str(&sb, pvr_proc_comm(), 1);
        sb_str(&sb, " si_code=", 1);
        sb_dec(&sb, si ? (long long)si->si_code : 0, 1);
        sb_str(&sb, " si_addr=", 1);
        sb_hex(&sb, (unsigned long long)fault_addr, 1);
        sb_str(&sb, " pc=", 1);
        sb_hex(&sb, (unsigned long long)pc, 1);
        sb_str(&sb, " lr=", 1);
        sb_hex(&sb, (unsigned long long)lr, 1);
        sb_str(&sb, " sp=", 1);
        sb_hex(&sb, (unsigned long long)sp, 1);
        sb_putc(&sb, '\n', 1);
        if (uc) {
            for (int r = 0; r < 30; r += 4) {
                sb_str(&sb, " ", 1);
                for (int c = 0; c < 4 && (r + c) < 30; c++) {
                    sb_str(&sb, " x", 1);
                    sb_dec(&sb, r + c, 1);
                    sb_putc(&sb, '=', 1);
                    sb_hex(&sb, (unsigned long long)uc->uc_mcontext.regs[r + c], 1);
                }
                sb_putc(&sb, '\n', 1);
            }
        }
        sig_dump_backtrace(&sb, pc, lr, sp, fp, fault_addr, "si_addr", 1);
        sb_flush(&sb, 1);
    }

    t_in_sig = 0;

    if (sig >= 0 && sig < 64 && g_prev_sa_valid[sig]) {
        struct sigaction *prev = &g_prev_sa[sig];
        if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction &&
            prev->sa_sigaction != icd_sig_handler) {
            prev->sa_sigaction(sig, si, uctx);
            return;
        }
        if (!(prev->sa_flags & SA_SIGINFO) &&
            prev->sa_handler != SIG_DFL &&
            prev->sa_handler != SIG_IGN &&
            prev->sa_handler != NULL &&
            prev->sa_handler != (void (*)(int))icd_sig_handler) {
            prev->sa_handler(sig);
            return;
        }
        if (prev->sa_handler == SIG_IGN) return;
    }

    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

static void icd_install_signal_handlers(void) {
    /* Never intercept SIGBUS/SIGSEGV/SIGILL/SIGTRAP in Wine/FEX/Proton unless explicitly asked:
     * FEXMemJIT and Wine rely on their own SA_SIGINFO handlers for unaligned atomics & guard pages. */
    const char *v = getenv("PVR_ICD_SIGLOG");
    if (!v || !*v || *v == '0') return;
    pthread_mutex_lock(&g_sig_mu);
    for (int i = 0; i < ICD_NUM_CAUGHT_SIGS; i++) {
        int sig = ICD_CAUGHT_SIGS[i];
        struct sigaction cur;
        if (sigaction(sig, NULL, &cur) == 0 && cur.sa_sigaction == icd_sig_handler)
            continue;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = icd_sig_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | (cur.sa_flags & (SA_RESTART | SA_ONSTACK));
        struct sigaction old;
        if (sigaction(sig, &sa, &old) == 0 && old.sa_sigaction != icd_sig_handler) {
            g_prev_sa[sig] = old;
            g_prev_sa_valid[sig] = 1;
        }
    }
    pthread_mutex_unlock(&g_sig_mu);
}

/* Minimal X11 / Xlib-xcb ABI declarations (libvulkan_powervr.so links -lX11 -lX11-xcb -lxcb)
 * without pulling X11/Xlib.h macros into icd code. */
typedef struct _XDisplay icd_Display;
typedef struct {
    int type;
    icd_Display *display;
    unsigned long resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
} icd_XErrorEvent;
typedef int (*icd_XErrorHandler)(icd_Display *, icd_XErrorEvent *);
typedef int (*icd_XIOErrorHandler)(icd_Display *);
extern icd_XErrorHandler XSetErrorHandler(icd_XErrorHandler);
extern icd_XIOErrorHandler XSetIOErrorHandler(icd_XIOErrorHandler);
extern int XGetErrorText(icd_Display *, int, char *, int);
extern int XConnectionNumber(icd_Display *);
extern void *XGetXCBConnection(icd_Display *);
extern int xcb_connection_has_error(void *);

static pthread_mutex_t g_x11_mu = PTHREAD_MUTEX_INITIALIZER;
static icd_XErrorHandler g_prev_xerr = NULL;
static icd_XIOErrorHandler g_prev_xioerr = NULL;

static int icd_x_error_handler(icd_Display *dpy, icd_XErrorEvent *ev) {
    char errtxt[256] = {0};
    if (dpy && ev) XGetErrorText(dpy, ev->error_code, errtxt, sizeof(errtxt));
    icd_wsi_logf("XError dpy=%p err=%u req=%u.%u res=0x%lx serial=%lu prev=%p text=\"%s\"",
                 (void *)dpy, ev ? ev->error_code : 0, ev ? ev->request_code : 0,
                 ev ? ev->minor_code : 0, ev ? ev->resourceid : 0UL,
                 ev ? ev->serial : 0UL, (void *)g_prev_xerr, errtxt);
    sig_buf_t sb;
    sb.len = 0;
    sb_str(&sb, "pvr_icd XError backtrace:\n", 1);
    sig_dump_backtrace(&sb, (uintptr_t)icd_x_error_handler,
                       (uintptr_t)__builtin_return_address(0), 0,
                       (uintptr_t)__builtin_frame_address(0),
                       (uintptr_t)g_prev_xerr, "prev_xerr", 1);
    sb_flush(&sb, 1);
    if (g_prev_xerr && g_prev_xerr != icd_x_error_handler)
        return g_prev_xerr(dpy, ev);
    return 0;
}

static int icd_x_io_error_handler(icd_Display *dpy) {
    int saved_errno = errno;
    void *xcb = dpy ? XGetXCBConnection(dpy) : NULL;
    int xcb_err = xcb ? xcb_connection_has_error(xcb) : -1;
    int fd = dpy ? XConnectionNumber(dpy) : -1;
    icd_wsi_logf("XIOError dpy=%p fd=%d errno=%d (%s) xcb=%p xcb_err=%d prev=%p",
                 (void *)dpy, fd, saved_errno, strerror(saved_errno),
                 xcb, xcb_err, (void *)g_prev_xioerr);
    sig_buf_t sb;
    sb.len = 0;
    sb_str(&sb, "pvr_icd XIOError backtrace:\n", 1);
    sig_dump_backtrace(&sb, (uintptr_t)icd_x_io_error_handler,
                       (uintptr_t)__builtin_return_address(0), 0,
                       (uintptr_t)__builtin_frame_address(0),
                       (uintptr_t)g_prev_xioerr, "prev_xioerr", 1);
    sb_flush(&sb, 1);
    if (g_prev_xioerr && g_prev_xioerr != icd_x_io_error_handler)
        return g_prev_xioerr(dpy);
    return 0;
}

static void icd_install_x11_handlers(const char *where) {
    pthread_mutex_lock(&g_x11_mu);
    icd_XErrorHandler old_err = XSetErrorHandler(icd_x_error_handler);
    if (old_err && old_err != icd_x_error_handler) g_prev_xerr = old_err;
    icd_XIOErrorHandler old_io = XSetIOErrorHandler(icd_x_io_error_handler);
    if (old_io && old_io != icd_x_io_error_handler) g_prev_xioerr = old_io;
    icd_wsi_logf("installed X11 handlers (%s): prev_xerr=%p prev_xioerr=%p",
                 where ? where : "?", (void *)g_prev_xerr, (void *)g_prev_xioerr);
    pthread_mutex_unlock(&g_x11_mu);
}

__attribute__((constructor)) static void pvr_icd_ctor(void) {
    pvr_proc_comm();
    { char b[300]; int n = snprintf(b, sizeof b, "pvr_proc pid=%d cmd=%s", (int)getpid(), pvr_proc_cmd());
      if (n > 0) { b[n] = 10; write(2, b, n + 1); } }
    icd_install_signal_handlers();
    /* WSI/GLComposer env vars (GAMESCOPE_WSI_FORCE_BYPASS, STEAM_GLCOMPOSER_USE_MAKECURRENT_LEGACY,
     * vk_x11_ignore_suboptimal) must be exported by droiddeck-session before launch rather than
     * via setenv() here, because libvulkan_powervr.so can be dlopen'd after worker threads start.
     * Also skip XInitThreads() inside steamwebhelper (unless PVR_ICD_XINITTHREADS=1) because
     * late XInitThreads() after XOpenDisplay() leaves earlier Displays with NULL lock_fns. */
    {
        const char *xit = getenv("PVR_ICD_XINITTHREADS");
        int do_xit = xit ? (*xit && *xit != '0') : (strstr(pvr_proc_cmd(), "steamwebhelper") == NULL);
        if (do_xit) {
            typedef int (*PFN_XInitThreads)(void);
            PFN_XInitThreads pXInitThreads = (PFN_XInitThreads)dlsym(RTLD_DEFAULT, "XInitThreads");
            if (!pXInitThreads) {
                void *x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
                if (x11) pXInitThreads = (PFN_XInitThreads)dlsym(x11, "XInitThreads");
            }
            if (pXInitThreads) pXInitThreads();
        }
    }
    Dl_info info;
    static char dir[4096], linker[4096];
    if (!getenv("HYBRIS_LD_LIBRARY_PATH"))
        setenv("HYBRIS_LD_LIBRARY_PATH",
               "/system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw", 1);
    if (!getenv("HYBRIS_LINKER_DIR") && dladdr((void *)pvr_icd_ctor, &info) && info.dli_fname) {
        snprintf(dir, sizeof(dir), "%s", info.dli_fname);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            snprintf(linker, sizeof(linker), "%s", dir);
            setenv("HYBRIS_LINKER_DIR", linker, 1);
            fprintf(stderr, "pvr_icd: HYBRIS_LINKER_DIR=%s\n", linker);
        }
    }
}

__attribute__((destructor)) static void pvr_icd_dtor(void) {
    const char *src_path = "/root/.local/share/Steam/logs/steamwebhelper.log";
    const char *dst_path = "/tmp/steamwebhelper_peak.log";
    struct stat st_src, st_dst;
    if (stat(src_path, &st_src) != 0 || st_src.st_size <= 1024) return;
    if (stat(dst_path, &st_dst) == 0 && st_dst.st_size >= st_src.st_size) return;
    int in_fd = open(src_path, O_RDONLY);
    if (in_fd < 0) return;
    int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) { close(in_fd); return; }
    char buf[8192];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));
            if (w <= 0) { off = n; break; }
            off += w;
        }
    }
    close(out_fd);
    close(in_fd);
}

/* Remember the HAL physical device handle so WSI surfaces that never received a surface caps query
 * (e.g. Gamescope WSI fallbackSurface) can still query VkPhysicalDeviceMemoryProperties. */
static void *g_last_phys_dev;

/* Resolve a dynamic Vulkan entrypoint through the HAL. The WSI (its own translation unit) uses this
 * instead of referencing the global vkGetDeviceProcAddr, which an ICD must not have undefined. */
static void *icd_gdpa(VkDevice device, const char *pName);   /* forward */
__attribute__((visibility("default")))
/* Memory properties for a physical device, through the HAL's own instance entrypoint: the WSI needs
 * them to pick a host-visible type for its readback buffer, and it only ever holds the
 * VkPhysicalDevice handed to vkGetPhysicalDeviceSurfaceCapabilitiesKHR. */
int pvr_icd_mem_props(void *pd, void *out) {
    if (!pd) pd = g_last_phys_dev;
    if (!pd || !out) return -1;
    if (!g_gipa && icd_load() != 0) return -1;
    void (*f)(void *, void *) = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!f) return -2;
    f(pd, out);
    return 0;
}

static void *icd_hal_dev_proc(void *device, const char *name) {
    if (!g_gipa && icd_load() != 0) return NULL;
    void *(*hal_gdpa)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
    if (!hal_gdpa) hal_gdpa = (void *(*)(void *, const char *))g_gipa(device, "vkGetDeviceProcAddr");
    return hal_gdpa ? hal_gdpa(device, name) : NULL;
}

typedef struct {
    void *mem;
    uint64_t size;
    void *placed_addr;
    uint64_t placed_size;
} IcdMemRec;

static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static IcdMemRec *g_mem_recs = NULL;
static size_t g_mem_count = 0, g_mem_cap = 0;

static void icd_mem_record_alloc(void *mem, uint64_t size) {
    if (!mem) return;
    pthread_mutex_lock(&g_mem_mu);
    for (size_t i = 0; i < g_mem_count; i++) {
        if (g_mem_recs[i].mem == mem) {
            g_mem_recs[i].size = size;
            g_mem_recs[i].placed_addr = NULL;
            g_mem_recs[i].placed_size = 0;
            pthread_mutex_unlock(&g_mem_mu);
            return;
        }
    }
    if (g_mem_count == g_mem_cap) {
        size_t nc = g_mem_cap ? g_mem_cap * 2 : 256;
        IcdMemRec *nr = (IcdMemRec *)realloc(g_mem_recs, nc * sizeof(IcdMemRec));
        if (nr) { g_mem_recs = nr; g_mem_cap = nc; }
    }
    if (g_mem_count < g_mem_cap) {
        g_mem_recs[g_mem_count++] = (IcdMemRec){ mem, size, NULL, 0 };
    }
    pthread_mutex_unlock(&g_mem_mu);
}

static int icd_mem_get(void *mem, IcdMemRec *out) {
    pthread_mutex_lock(&g_mem_mu);
    for (size_t i = 0; i < g_mem_count; i++) {
        if (g_mem_recs[i].mem == mem) {
            if (out) *out = g_mem_recs[i];
            pthread_mutex_unlock(&g_mem_mu);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_mem_mu);
    return 0;
}

static void icd_mem_set_placed(void *mem, void *addr, uint64_t size) {
    pthread_mutex_lock(&g_mem_mu);
    for (size_t i = 0; i < g_mem_count; i++) {
        if (g_mem_recs[i].mem == mem) {
            g_mem_recs[i].placed_addr = addr;
            g_mem_recs[i].placed_size = size;
            break;
        }
    }
    pthread_mutex_unlock(&g_mem_mu);
}

static void icd_mem_remove(void *mem, IcdMemRec *out) {
    pthread_mutex_lock(&g_mem_mu);
    for (size_t i = 0; i < g_mem_count; i++) {
        if (g_mem_recs[i].mem == mem) {
            if (out) *out = g_mem_recs[i];
            g_mem_recs[i] = g_mem_recs[--g_mem_count];
            pthread_mutex_unlock(&g_mem_mu);
            return;
        }
    }
    pthread_mutex_unlock(&g_mem_mu);
    if (out) memset(out, 0, sizeof(*out));
}

static uint32_t icd_host_visible_mask(void) {
    static uint32_t cached = 0;
    static int have = 0;
    if (have) return cached;
    struct { uint32_t count; struct { uint32_t flags; uint32_t heap; } types[32]; uint32_t rest[64]; } mp;
    memset(&mp, 0, sizeof(mp));
    if (pvr_icd_mem_props(NULL, &mp) == 0 && mp.count <= 32) {
        uint32_t m = 0;
        for (uint32_t i = 0; i < mp.count; i++) {
            if (mp.types[i].flags & 0x2u) m |= (1u << i); /* VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT */
        }
        cached = m;
        have = 1;
    }
    return cached;
}

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    uint64_t allocationSize; uint32_t memoryTypeIndex; uint32_t pad2;
} IcdMemAllocInfo;

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    uint32_t handleTypes; uint32_t pad2;
} IcdExportMemAllocInfo;

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    void *memory; uint32_t handleType; uint32_t pad2;
} IcdMemGetFdInfoKHR;

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    uint32_t flags; uint32_t pad2; void *memory;
    uint64_t offset; uint64_t size;
} IcdMemMapInfoKHR;

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    void *pPlacedAddress;
} IcdMemMapPlacedInfoEXT;

typedef struct {
    uint32_t sType; uint32_t pad; const void *pNext;
    uint32_t flags; uint32_t pad2; void *memory;
} IcdMemUnmapInfoKHR;

static VkResult icd_allocate_memory(void *device, const void *pAllocateInfo, const void *pAllocator, void **pMemory) {
    VkResult (*hal)(void *, const void *, const void *, void **) =
        (VkResult (*)(void *, const void *, const void *, void **))icd_hal_dev_proc(device, "vkAllocateMemory");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;
    const IcdMemAllocInfo *ai = (const IcdMemAllocInfo *)pAllocateInfo;
    VkResult r = VK_ERROR_UNKNOWN;
    if (ai && ((1u << ai->memoryTypeIndex) & icd_host_visible_mask())) {
        int has_exp = 0;
        for (const uint8_t *p = (const uint8_t *)ai->pNext; p; p = *(const uint8_t *const *)(p + 8)) {
            if (*(const uint32_t *)p == 1000072002u) { has_exp = 1; break; }
        }
        if (!has_exp) {
            IcdMemAllocInfo copy = *ai;
            IcdExportMemAllocInfo exp = { 1000072002u, 0, ai->pNext, 0x00000200u /* DMA_BUF_BIT_EXT */, 0 };
            copy.pNext = &exp;
            r = hal(device, &copy, pAllocator, pMemory);
        }
    }
    if (r != VK_SUCCESS) {
        r = hal(device, pAllocateInfo, pAllocator, pMemory);
    }
    if (r == VK_SUCCESS && ai && pMemory && *pMemory) {
        icd_mem_record_alloc(*pMemory, ai->allocationSize);
    }
    return r;
}

static void icd_unmap_placed_if_any(const IcdMemRec *rec, int reserve) {
    if (!rec->placed_addr || !rec->placed_size) return;
    if (reserve) {
        mmap(rec->placed_addr, (size_t)rec->placed_size, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        munmap(rec->placed_addr, (size_t)rec->placed_size);
    }
}

static void icd_free_memory(void *device, void *memory, const void *pAllocator) {
    if (memory) {
        IcdMemRec rec;
        icd_mem_remove(memory, &rec);
        icd_unmap_placed_if_any(&rec, 0);
    }
    void (*hal)(void *, void *, const void *) =
        (void (*)(void *, void *, const void *))icd_hal_dev_proc(device, "vkFreeMemory");
    if (hal) hal(device, memory, pAllocator);
}

static VkResult icd_map_memory(void *device, void *memory, uint64_t offset, uint64_t size, uint32_t flags, void **ppData) {
    VkResult (*hal)(void *, void *, uint64_t, uint64_t, uint32_t, void **) =
        (VkResult (*)(void *, void *, uint64_t, uint64_t, uint32_t, void **))icd_hal_dev_proc(device, "vkMapMemory");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;
    return hal(device, memory, offset, size, flags, ppData);
}

static void icd_unmap_memory(void *device, void *memory) {
    IcdMemRec rec;
    if (memory && icd_mem_get(memory, &rec) && rec.placed_addr) {
        icd_unmap_placed_if_any(&rec, 0);
        icd_mem_set_placed(memory, NULL, 0);
        return;
    }
    void (*hal)(void *, void *) = (void (*)(void *, void *))icd_hal_dev_proc(device, "vkUnmapMemory");
    if (hal) hal(device, memory);
}

static VkResult icd_map_memory2(void *device, const void *pMemoryMapInfo, void **ppData) {
    const IcdMemMapInfoKHR *info = (const IcdMemMapInfoKHR *)pMemoryMapInfo;
    if (!info || !ppData) return VK_ERROR_INITIALIZATION_FAILED;
    if (info->flags & 0x1u /* VK_MEMORY_MAP_PLACED_BIT_EXT */) {
        void *placed_addr = NULL;
        for (const uint8_t *p = (const uint8_t *)info->pNext; p; p = *(const uint8_t *const *)(p + 8)) {
            if (*(const uint32_t *)p == 1000272002u) {
                placed_addr = ((const IcdMemMapPlacedInfoEXT *)p)->pPlacedAddress;
                break;
            }
        }
        if (!placed_addr) return VK_ERROR_MEMORY_MAP_FAILED;
        IcdMemRec rec = {0};
        if (!icd_mem_get(info->memory, &rec)) return VK_ERROR_MEMORY_MAP_FAILED;
        uint64_t map_size = (info->size == UINT64_MAX) ? (rec.size - info->offset) : info->size;
        VkResult (*get_fd)(void *, const void *, int *) =
            (VkResult (*)(void *, const void *, int *))icd_hal_dev_proc(device, "vkGetMemoryFdKHR");
        if (!get_fd) return VK_ERROR_MEMORY_MAP_FAILED;
        int fd = -1;
        IcdMemGetFdInfoKHR fi = { 1000074002u, 0, NULL, info->memory, 0x00000200u /* DMA_BUF_BIT_EXT */, 0 };
        VkResult fr = get_fd(device, &fi, &fd);
        if (fr != VK_SUCCESS || fd < 0) {
            fi.handleType = 0x00000001u; /* OPAQUE_FD_BIT */
            fr = get_fd(device, &fi, &fd);
        }
        if (fr != VK_SUCCESS || fd < 0) {
            icd_wsi_logf("vkMapMemory2 placed: vkGetMemoryFdKHR failed rc=%d", fr);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        void *m = mmap(placed_addr, (size_t)map_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)info->offset);
        int saved_errno = errno;
        close(fd);
        if (m == MAP_FAILED) {
            icd_wsi_logf("vkMapMemory2 placed: mmap(%p, %llu, off=%llu) failed errno=%d",
                         placed_addr, (unsigned long long)map_size, (unsigned long long)info->offset, saved_errno);
            return VK_ERROR_MEMORY_MAP_FAILED;
        }
        icd_mem_set_placed(info->memory, m, map_size);
        *ppData = m;
        return VK_SUCCESS;
    }
    return icd_map_memory(device, info->memory, info->offset, info->size, info->flags, ppData);
}

static VkResult icd_unmap_memory2(void *device, const void *pMemoryUnmapInfo) {
    const IcdMemUnmapInfoKHR *info = (const IcdMemUnmapInfoKHR *)pMemoryUnmapInfo;
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    IcdMemRec rec;
    if (info->memory && icd_mem_get(info->memory, &rec) && rec.placed_addr) {
        icd_unmap_placed_if_any(&rec, (info->flags & 0x1u) != 0);
        icd_mem_set_placed(info->memory, NULL, 0);
        return VK_SUCCESS;
    }
    icd_unmap_memory(device, info->memory);
    return VK_SUCCESS;
}

static void icd_get_phys_dev_features(void *pd, void *features);
static void icd_get_phys_dev_features2(void *pd, void *features2);

void *pvr_icd_resolve(void *instance, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)icd_gdpa;
    if (!strcmp(name, "vkGetPhysicalDeviceProcAddr")) return (void *)icd_gpdpa;
    { void *sp = pvr_wsi_sync_proc(name); if (sp) return sp; }
    if (!strcmp(name, "vkCreateImage")) return (void *)icd_create_image;
    if (!strcmp(name, "vkCreateShaderModule")) return (void *)icd_create_shader_module;
    if (!strcmp(name, "vkCreateComputePipelines")) return (void *)icd_create_compute_pipelines;
    if (!strcmp(name, "vkCreateGraphicsPipelines")) return (void *)icd_create_graphics_pipelines;
    if (!strcmp(name, "vkAllocateMemory")) return (void *)icd_allocate_memory;
    if (!strcmp(name, "vkFreeMemory")) return (void *)icd_free_memory;
    if (!strcmp(name, "vkMapMemory")) return (void *)icd_map_memory;
    if (!strcmp(name, "vkUnmapMemory")) return (void *)icd_unmap_memory;
    if (!strcmp(name, "vkMapMemory2") || !strcmp(name, "vkMapMemory2KHR")) return (void *)icd_map_memory2;
    if (!strcmp(name, "vkUnmapMemory2") || !strcmp(name, "vkUnmapMemory2KHR")) return (void *)icd_unmap_memory2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))
        return (void *)icd_get_phys_dev_features;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (void *)icd_get_phys_dev_features2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (void *)icd_get_phys_dev_props2;
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2") || !strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (void *)icd_get_phys_dev_img_fmt_props2;
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa((VkInstance)instance, name);
}

/* Diagnostic: the HAL's PLT at open, so it can be compared with the same table at a later crash.
 * The question is whether a slot's value CHANGES during the process's life (something rewrites it)
 * or was already wrong when the HAL was opened. Off unless PVR_ICD_DIAG names a file to append to. */
#define HAL_OPEN_VADDR   0x1b9ee0u
#define HAL_GOT_PLT_OFF  0x286ab0u
#define HAL_GOT_PLT_N    349
static FILE *diagf(void) {
    static FILE *f; static int tried;
    if (!tried) { const char *p = getenv("PVR_ICD_DIAG"); tried = 1; if (p) f = fopen(p, "a"); }
    return f;
}
/* Every symbol libhybris is asked to resolve for the HAL, and (through diag_plt) what the HAL's PLT
 * ends up holding. The callback is libhybris's documented seam; setting it here is safe because the
 * ICD loads the HAL, so it is registered before any of the HAL's symbols are resolved. NULL means
 * "resolve it normally". */
typedef void *(*hook_cb_t)(const char *name, const char *requester);
extern void hybris_set_hook_callback(hook_cb_t callback);
static void *diag_hook(const char *name, const char *requester) {
    FILE *o = diagf();
    if (o && name) {
        fprintf(o, "ICD hook pid=%d sym=%s requester=%s\n",
                (int)getpid(), name, requester ? requester : "(null)");
        fflush(o);
    }
    return NULL;
}

static void diag_plt(unsigned long long base, const char *when) {
    FILE *o = diagf();
    if (!o || !base) return;
    char cb[64] = {0};
    int cf = open("/proc/self/comm", O_RDONLY);
    if (cf >= 0) { ssize_t r = read(cf, cb, sizeof(cb) - 1); (void)r; close(cf); }
    for (char *q = cb; *q; q++) if (*q == 10) { *q = 0; break; }
    unsigned long long *g = (unsigned long long *)(uintptr_t)(base + HAL_GOT_PLT_OFF);
    fprintf(o, "ICD %s pid=%d comm=%s base=%llx", when, (int)getpid(), cb, base);
    static const int w[] = {13,14,30,57,68,70,73,75,76,77,167,168,197,198,220,348,349};
    for (unsigned k = 0; k < sizeof(w)/sizeof(w[0]); k++)
        fprintf(o, " %d=%llx", w[k], g[w[k]]);
    fprintf(o, "\n");
    fflush(o);
}

/* Load the bionic HAL once. Returns 0 on success. */
static int icd_load(void) {
    if (g_gipa) return 0;
    if (diagf()) hybris_set_hook_callback(diag_hook);   /* before the HAL's symbols are resolved */
    g_blob = android_dlopen_ext(BLOB, RTLD_NOW, NULL);
    if (!g_blob) { fprintf(stderr, "pvr_icd: android_dlopen_ext(%s) failed: %s\n", BLOB, android_dlerror()); return -1; }

    hw_module_t *mod = (hw_module_t *)android_dlsym(g_blob, "HMI");
    if (!mod || !mod->methods || !mod->methods->open) { icd_log("no HMI/open in the HAL"); return -1; }

    { Dl_info hi; unsigned long long hb = 0;
      if (dladdr((void *)mod->methods->open, &hi) && hi.dli_fbase)
          hb = (unsigned long long)(uintptr_t)hi.dli_fbase;
      /* dladdr cannot see a libhybris-loaded library, so fall back to the known file offset of the
       * entry point (vulkan_device_open is at 0x1b9ee0). */
      if (!hb) hb = (unsigned long long)(uintptr_t)mod->methods->open - HAL_OPEN_VADDR;
      diag_plt(hb, "before_open"); }
    void *dev = NULL;
    int rc = mod->methods->open(mod, "vk0", &dev);
    { unsigned long long hb = (unsigned long long)(uintptr_t)mod->methods->open - HAL_OPEN_VADDR;
      diag_plt(hb, "after_open"); }
    if (rc != 0 || !dev) { fprintf(stderr, "pvr_icd: HAL open(vk0) rc=%d\n", rc); return -1; }

    hwvulkan_device_t *d = (hwvulkan_device_t *)dev;
    g_gipa = (void *(*)(VkInstance, const char *))d->w[0x88 / 8];
    g_create = (void *(*)(const void *, const void *, VkInstance *))d->w[0x80 / 8];
    if (!g_gipa) { icd_log("HAL device has no GetInstanceProcAddr"); return -1; }
    return 0;
}

/* ---- ICD interface ---------------------------------------------------------- */

/* Device entrypoints: ours (the WSI) first, then the HAL's. The loader consults this to decide
 * whether VK_KHR_swapchain is supported, so vkCreateSwapchainKHR/vkQueuePresentKHR must be here. */
/* VkDeviceCreateInfo, spelled out so we can rewrite the extension list in place. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const void *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} DeviceCreateInfo;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueFamilyIndex; uint32_t queueCount;
    const float *pQueuePriorities;
} DeviceQueueCreateInfo;

/* vkCreateDevice: our WSI provides the swapchain, not the HAL. The HAL's own swapchain is
 * Android-surface based and fails to initialise in a glibc guest, so we must not enable
 * VK_KHR_swapchain on the HAL device -- keep it for the app (our vkCreateSwapchainKHR serves it)
 * and drop it on the way to the HAL. */
static VkResult icd_create_device(void *pd, const void *ci, const void *alloc, void **out) {
    icd_install_signal_handlers();
    icd_install_x11_handlers("vkCreateDevice");
    if (pd) g_last_phys_dev = pd;
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal)(void *, const void *, const void *, void **) =
        (VkResult (*)(void *, const void *, const void *, void **))g_gipa(g_instance, "vkCreateDevice");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceCreateInfo *in = (const DeviceCreateInfo *)ci;
    uint32_t n = in->enabledExtensionCount;
    const char *const *names = in->ppEnabledExtensionNames;
    /* Our WSI needs the external-memory handles on the HAL device to export swapchain dma-bufs. */
    static const char *const need[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf" };
    int have[2] = { 0, 0 };
    for (uint32_t i = 0; i < n; i++)
        for (int k = 0; k < 2; k++) if (!strcmp(names[i], need[k])) have[k] = 1;
    const char **kept = (const char **)malloc(sizeof(char *) * (n + 2));
    uint32_t kn = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(names[i], "VK_KHR_swapchain") ||
            !strcmp(names[i], "VK_KHR_map_memory2") ||
            !strcmp(names[i], "VK_EXT_map_memory_placed")) continue;
        kept[kn++] = names[i];
    }
    for (int k = 0; k < 2; k++) if (!have[k]) kept[kn++] = need[k];
    DeviceCreateInfo copy = *in;
    copy.enabledExtensionCount = kn;
    copy.ppEnabledExtensionNames = kept;
    /* PowerVR HAL leaves VkPhysicalDeviceFeatures.textureCompressionBC=0 (missing BC6H/BC7)
     * even though BC1..BC5 are natively supported; strip the emulated bit before the HAL sees it. */
    uint32_t feat_copy[55];
    if (in->pEnabledFeatures) {
        memcpy(feat_copy, in->pEnabledFeatures, sizeof(feat_copy));
        feat_copy[22] = 0; /* textureCompressionBC */
        copy.pEnabledFeatures = feat_copy;
    }
    typedef struct { uint32_t sType; uint32_t pad; const void *pNext; uint32_t feats[55]; } Feat2Node;
    typedef struct { uint32_t sType; uint32_t pad; const void *pNext; } VkChainHdr;
    const void *orig_f2 = NULL;
    uint32_t orig_bc2 = 0;
    VkChainHdr *placed_prev = NULL;
    const void *placed_node = NULL;
    const void *placed_next = NULL;
    for (uint8_t *p = (uint8_t *)copy.pNext, *prev = NULL; p; ) {
        uint32_t st = *(const uint32_t *)p;
        uint8_t *nxt = (uint8_t *)((const VkChainHdr *)p)->pNext;
        if (st == 1000059000u && ((const Feat2Node *)p)->feats[22]) {
            orig_f2 = p;
            orig_bc2 = ((const Feat2Node *)p)->feats[22];
            ((Feat2Node *)p)->feats[22] = 0;
        } else if (st == 1000272000u /* VkPhysicalDeviceMapMemoryPlacedFeaturesEXT */ && !placed_node) {
            placed_prev = (VkChainHdr *)prev;
            placed_node = p;
            placed_next = nxt;
            if (placed_prev) placed_prev->pNext = nxt;
            else copy.pNext = nxt;
            p = nxt;
            continue;
        }
        prev = p;
        p = nxt;
    }
    VkResult r = hal(pd, &copy, alloc, out);
    if (orig_f2) ((Feat2Node *)orig_f2)->feats[22] = orig_bc2;
    if (placed_node && placed_prev) placed_prev->pNext = placed_node;
    (void)placed_next;
    free(kept);
    if (r != VK_SUCCESS) return r;
    /* The WSI drives its own queue submissions to complete a frame's fences and semaphores, so it
     * needs the HAL's device entrypoints -- and the queue family the app's queues come from, taken
     * from the first VkDeviceQueueCreateInfo (this device has one general family). */
    if (out && *out) {
        uint32_t qf = 0;
        if (in->queueCreateInfoCount > 0 && in->pQueueCreateInfos) {
            const DeviceQueueCreateInfo *q0 = (const DeviceQueueCreateInfo *)in->pQueueCreateInfos;
            qf = q0->queueFamilyIndex;
        }
        void *(*gdpa)(void *, const char *) =
            (void *(*)(void *, const char *))g_gipa(*out, "vkGetDeviceProcAddr");
        if (gdpa) {
            pvr_wsi_register_device(*out, qf, gdpa(*out, "vkQueueSubmit"), gdpa(*out, "vkCreateFence"),
                                    gdpa(*out, "vkWaitForFences"), gdpa(*out, "vkDestroyFence"),
                                    gdpa(*out, "vkGetDeviceQueue"),
                                    gdpa(*out, "vkQueueSubmit2"), gdpa(*out, "vkQueueSubmit2KHR"),
                                    gdpa(*out, "vkQueueWaitIdle"), gdpa(*out, "vkDeviceWaitIdle"),
                                    gdpa(*out, "vkGetDeviceQueue2"), gdpa(*out, "vkDestroyDevice"));
        }
    }
    return r;
}

/* Per-present command accounting.
 *
 * The readback says the frame is black; the layer API says the swapchain exists and presents. This
 * counts what the client actually records, so the question "does it ever draw into the picture, or
 * only clear it" is answered with numbers instead of inference. Vulkan command entrypoints take no
 * device, so the device the app queries is remembered here (one per process in practice) purely to
 * resolve the HAL's real function for each wrapper.
 */
static void *(*g_cmd_hal_gdpa)(VkDevice, const char *);
static VkDevice g_cmd_dev;
unsigned long g_cmd_draw, g_cmd_drawi, g_cmd_dispatch, g_cmd_beginrp, g_cmd_beginrend,
              g_cmd_clearimg, g_cmd_clearatt, g_cmd_copyimg, g_cmd_copybuf, g_cmd_blit;

static void *cmd_hal(const char *name) {
    if (!g_cmd_hal_gdpa) {
        if (!g_gipa && icd_load() != 0) return NULL;
        g_cmd_hal_gdpa = (void *(*)(VkDevice, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
    }
    return g_cmd_hal_gdpa ? g_cmd_hal_gdpa(g_cmd_dev, name) : NULL;
}

static void cmd_draw(void *cb, uint32_t vc, uint32_t ic2, uint32_t fv, uint32_t fi) {
    g_cmd_draw++;
    void (*f)(void *, uint32_t, uint32_t, uint32_t, uint32_t) = (void (*)(void *, uint32_t, uint32_t, uint32_t, uint32_t))cmd_hal("vkCmdDraw");
    if (f) f(cb, vc, ic2, fv, fi);
}
static void cmd_drawi(void *cb, uint32_t ic2, uint32_t inst, uint32_t fi, int32_t vo, uint32_t finst) {
    g_cmd_drawi++;
    void (*f)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) = (void (*)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t))cmd_hal("vkCmdDrawIndexed");
    if (f) f(cb, ic2, inst, fi, vo, finst);
}
static void cmd_dispatch(void *cb, uint32_t x, uint32_t y, uint32_t z) {
    g_cmd_dispatch++;
    void (*f)(void *, uint32_t, uint32_t, uint32_t) = (void (*)(void *, uint32_t, uint32_t, uint32_t))cmd_hal("vkCmdDispatch");
    if (f) f(cb, x, y, z);
}
static void cmd_beginrp(void *cb, const void *bi, uint32_t contents) {
    g_cmd_beginrp++;
    void (*f)(void *, const void *, uint32_t) = (void (*)(void *, const void *, uint32_t))cmd_hal("vkCmdBeginRenderPass");
    if (f) f(cb, bi, contents);
}
static void cmd_beginrend(void *cb, const void *ri) {
    g_cmd_beginrend++;
    static int br_logged;
    if (ri && br_logged < 12) {
        br_logged++;
        const uint32_t *u = (const uint32_t *)ri;
        /* VkRenderingInfo: sType(0), pNext(8), flags(16), renderArea(20..35),
         * layerCount(36), viewMask(40), colorAttachmentCount(44), pColorAttachments(48) */
        const int32_t *ra = (const int32_t *)((const char *)ri + 20);
        uint32_t flags = u[4];
        uint32_t layers = u[9], vmask = u[10], ncolor = u[11];
        const void *const *ptrs = (const void *const *)((const char *)ri + 48);
        const char *pcolor = (const char *)ptrs[0];
        const void *pdepth = ptrs[1];
        const void *pstencil = ptrs[2];
        if (ncolor > 0 && pcolor) {
            const void *const *cv_ptrs = (const void *const *)(pcolor + 16);
            const uint32_t *cu = (const uint32_t *)pcolor;
            const float *cf = (const float *)(pcolor + 52);
            fprintf(stderr, "pvr_beginrend[%d] pid=%d flags=0x%x area=(%d,%d %dx%d) layers=%u ncolor=%u depth=%d stencil=%d "
                            "view0=%p layout0=%u resMode=%u resView=%p loadOp=%u storeOp=%u clear=(%.2f,%.2f,%.2f,%.2f)\n",
                    br_logged, (int)getpid(), flags, ra[0], ra[1], ra[2], ra[3], layers, ncolor,
                    pdepth != NULL, pstencil != NULL,
                    cv_ptrs[0], cu[6], cu[7], cv_ptrs[2], cu[11], cu[12],
                    cf[0], cf[1], cf[2], cf[3]);
        } else {
            fprintf(stderr, "pvr_beginrend[%d] pid=%d flags=0x%x area=(%d,%d %dx%d) layers=%u ncolor=%u depth=%d stencil=%d\n",
                    br_logged, (int)getpid(), flags, ra[0], ra[1], ra[2], ra[3], layers, ncolor,
                    pdepth != NULL, pstencil != NULL);
        }
        fflush(stderr);
    }
    void (*f)(void *, const void *) = (void (*)(void *, const void *))cmd_hal("vkCmdBeginRendering");
    if (f) f(cb, ri);
}
static void cmd_clearimg(void *cb, void *img, uint32_t layout, const void *col, uint32_t nr, const void *ranges) {
    g_cmd_clearimg++;
    void (*f)(void *, void *, uint32_t, const void *, uint32_t, const void *) = (void (*)(void *, void *, uint32_t, const void *, uint32_t, const void *))cmd_hal("vkCmdClearColorImage");
    if (f) f(cb, img, layout, col, nr, ranges);
}
static void cmd_clearatt(void *cb, uint32_t natt, const void *att, uint32_t nrect, const void *rects) {
    g_cmd_clearatt++;
    void (*f)(void *, uint32_t, const void *, uint32_t, const void *) = (void (*)(void *, uint32_t, const void *, uint32_t, const void *))cmd_hal("vkCmdClearAttachments");
    if (f) f(cb, natt, att, nrect, rects);
}
static void *g_copy_src, *g_copy_dst;   /* endpoints of the last vkCmdCopyImage */
static void cmd_copyimg(void *cb, void *src, uint32_t sl, void *dst, uint32_t dl, uint32_t nr, const void *regions) {
    g_cmd_copyimg++;
    g_copy_src = src; g_copy_dst = dst;
    void (*f)(void *, void *, uint32_t, void *, uint32_t, uint32_t, const void *) = (void (*)(void *, void *, uint32_t, void *, uint32_t, uint32_t, const void *))cmd_hal("vkCmdCopyImage");
    if (f) f(cb, src, sl, dst, dl, nr, regions);
}
static void cmd_copybuf(void *cb, void *buf, void *img, uint32_t layout, uint32_t nr, const void *regions) {
    g_cmd_copybuf++;
    void (*f)(void *, void *, void *, uint32_t, uint32_t, const void *) = (void (*)(void *, void *, void *, uint32_t, uint32_t, const void *))cmd_hal("vkCmdCopyBufferToImage");
    if (f) f(cb, buf, img, layout, nr, regions);
}
static void cmd_blit(void *cb, void *src, uint32_t sl, void *dst, uint32_t dl, uint32_t nr, const void *regions, uint32_t filter) {
    g_cmd_blit++;
    void (*f)(void *, void *, uint32_t, void *, uint32_t, uint32_t, const void *, uint32_t) = (void (*)(void *, void *, uint32_t, void *, uint32_t, uint32_t, const void *, uint32_t))cmd_hal("vkCmdBlitImage");
    if (f) f(cb, src, sl, dst, dl, nr, regions, filter);
}

/* Image create info, kept so a probe can state the format/tiling/usage/extent of an image it is
 * handed only as a handle (the copy source belongs to CEF, not to the WSI). */
typedef struct { void *img; uint32_t fmt, tiling, usage, w, h; int external; } pvr_img_info;
static pvr_img_info g_imginfo[256];
static int g_nimginfo;
static pthread_mutex_t g_imginfo_mu = PTHREAD_MUTEX_INITIALIZER;

void pvr_icd_note_image(void *img, uint32_t fmt, uint32_t tiling, uint32_t usage, uint32_t w, uint32_t h, int external) {
    if (!img) return;
    /* Ring, not a cap: CEF creates far more than 256 images per session, and a plain cap left its
     * present-time UI image untracked (probe skipped for want of its extent). */
    pthread_mutex_lock(&g_imginfo_mu);
    int i = (int)((uint32_t)(g_nimginfo++) % 256u);
    g_imginfo[i].img = img; g_imginfo[i].fmt = fmt;
    g_imginfo[i].tiling = tiling; g_imginfo[i].usage = usage;
    g_imginfo[i].w = w; g_imginfo[i].h = h; g_imginfo[i].external = external;
    pthread_mutex_unlock(&g_imginfo_mu);
}
int pvr_icd_img_info(void *img, uint32_t *out5) {
    pthread_mutex_lock(&g_imginfo_mu);
    int limit = g_nimginfo < 256 ? g_nimginfo : 256;
    for (int i = limit - 1; i >= 0; i--)
        if (g_imginfo[i].img == img) {
            out5[0] = g_imginfo[i].fmt; out5[1] = g_imginfo[i].tiling; out5[2] = g_imginfo[i].usage;
            out5[3] = g_imginfo[i].w; out5[4] = g_imginfo[i].h;
            int ext = g_imginfo[i].external;
            pthread_mutex_unlock(&g_imginfo_mu);
            return ext;
        }
    pthread_mutex_unlock(&g_imginfo_mu);
    return -1;
}
void *pvr_icd_last_copy(int which) { return which ? g_copy_dst : g_copy_src; }

static void *pvr_icd_cmd_proc(VkDevice device, const char *name) {
    static int trace_enabled = -1;
    if (trace_enabled < 0) {
        const char *e = getenv("PVR_ICD_CMD_TRACE");
        trace_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    if (!trace_enabled || !name) return NULL;
    if (device) g_cmd_dev = (VkDevice)device;
    if (!strcmp(name, "vkCmdDraw")) return (void *)cmd_draw;
    if (!strcmp(name, "vkCmdDrawIndexed")) return (void *)cmd_drawi;
    if (!strcmp(name, "vkCmdDispatch")) return (void *)cmd_dispatch;
    if (!strcmp(name, "vkCmdBeginRenderPass")) return (void *)cmd_beginrp;
    if (!strcmp(name, "vkCmdBeginRendering")) return (void *)cmd_beginrend;
    if (!strcmp(name, "vkCmdBeginRenderingKHR")) return (void *)cmd_beginrend;
    if (!strcmp(name, "vkCmdClearColorImage")) return (void *)cmd_clearimg;
    if (!strcmp(name, "vkCmdClearAttachments")) return (void *)cmd_clearatt;
    if (!strcmp(name, "vkCmdCopyImage")) return (void *)cmd_copyimg;
    if (!strcmp(name, "vkCmdCopyBufferToImage")) return (void *)cmd_copybuf;
    if (!strcmp(name, "vkCmdBlitImage")) return (void *)cmd_blit;
    return NULL;
}

/* Dump and reset, called from the WSI at present time for the real surface only. */
void pvr_icd_cmd_report(const char *tag, int w, int h) {
    static int n;
    if (!tag) return;
    n++;
    fprintf(stderr, "pvr_cmd[%s] %dx%d #%d draw=%lu drawi=%lu dispatch=%lu beginrp=%lu beginrend=%lu "
                    "clearimg=%lu clearatt=%lu copyimg=%lu copybuf=%lu blit=%lu\n",
            tag, w, h, n, g_cmd_draw, g_cmd_drawi, g_cmd_dispatch, g_cmd_beginrp, g_cmd_beginrend,
            g_cmd_clearimg, g_cmd_clearatt, g_cmd_copyimg, g_cmd_copybuf, g_cmd_blit);
    fflush(stderr);
    g_cmd_draw = g_cmd_drawi = g_cmd_dispatch = g_cmd_beginrp = g_cmd_beginrend = 0;
    g_cmd_clearimg = g_cmd_clearatt = g_cmd_copyimg = g_cmd_copybuf = g_cmd_blit = 0;
}

static void *icd_gdpa(VkDevice device, const char *pName) {
    /* Diagnostic: EVERY device entry point the app asks for, at entry, so a NULL answer is visible
     * even for the names this ICD serves itself (which return before the tail log). A NULL becomes a
     * NULL slot in the app's dispatch table and the app calls it and faults. Off unless PVR_ICD_DIAG. */
    FILE *o = diagf();
    if (o && pName) { fprintf(o, "ICD gdpa pid=%d name=%s", (int)getpid(), pName); fflush(o); }
    if (!strcmp(pName, "vkCreateImage")) { if (o) { fprintf(o, " -> icd_create_image\n"); fflush(o); } return (void *)icd_create_image; }
    if (!strcmp(pName, "vkCreateShaderModule")) { if (o) { fprintf(o, " -> icd_create_shader_module\n"); fflush(o); } return (void *)icd_create_shader_module; }
    if (!strcmp(pName, "vkCreateComputePipelines")) { if (o) { fprintf(o, " -> icd_create_compute_pipelines\n"); fflush(o); } return (void *)icd_create_compute_pipelines; }
    if (!strcmp(pName, "vkCreateGraphicsPipelines")) { if (o) { fprintf(o, " -> icd_create_graphics_pipelines\n"); fflush(o); } return (void *)icd_create_graphics_pipelines; }
    if (!strcmp(pName, "vkAllocateMemory")) { if (o) { fprintf(o, " -> icd_allocate_memory\n"); fflush(o); } return (void *)icd_allocate_memory; }
    if (!strcmp(pName, "vkFreeMemory")) { if (o) { fprintf(o, " -> icd_free_memory\n"); fflush(o); } return (void *)icd_free_memory; }
    if (!strcmp(pName, "vkMapMemory")) { if (o) { fprintf(o, " -> icd_map_memory\n"); fflush(o); } return (void *)icd_map_memory; }
    if (!strcmp(pName, "vkUnmapMemory")) { if (o) { fprintf(o, " -> icd_unmap_memory\n"); fflush(o); } return (void *)icd_unmap_memory; }
    if (!strcmp(pName, "vkMapMemory2") || !strcmp(pName, "vkMapMemory2KHR")) { if (o) { fprintf(o, " -> icd_map_memory2\n"); fflush(o); } return (void *)icd_map_memory2; }
    if (!strcmp(pName, "vkUnmapMemory2") || !strcmp(pName, "vkUnmapMemory2KHR")) { if (o) { fprintf(o, " -> icd_unmap_memory2\n"); fflush(o); } return (void *)icd_unmap_memory2; }
    { void *cmd = pvr_icd_cmd_proc(device, pName);
      if (cmd) { if (o) { fprintf(o, " -> cmdstat\n"); fflush(o); } return cmd; } }
    void *wsi = pvr_wsi_proc(pName);
    if (wsi) { if (o) { fprintf(o, " -> %p (wsi)\n", wsi); fflush(o); } return wsi; }
    void *wsi_sync = pvr_wsi_sync_proc(pName);
    if (wsi_sync) { if (o) { fprintf(o, " -> %p (wsi-sync)\n", wsi_sync); fflush(o); } return wsi_sync; }
    if (!g_gipa && icd_load() != 0) { if (o) { fprintf(o, " -> load-fail\n"); fflush(o); } return NULL; }
    void *(*hal_gdpa)(VkDevice, const char *) =
        (void *(*)(VkDevice, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
    void *r = hal_gdpa ? hal_gdpa(device, pName) : NULL;
    if (o) { fprintf(o, " -> %p\n", r); fflush(o); }
    return r;
}

/* Device extensions this ICD adds on top of the HAL: the swapchain the WSI presents to, and the
 * external-memory handles its buffers use. The loader validates requested device extensions against
 * this list, so it must include them. */
static const char *const ICD_DEVICE_EXTENSIONS[] = {
    "VK_KHR_swapchain", "VK_KHR_external_memory", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
    "VK_KHR_map_memory2", "VK_EXT_map_memory_placed",
};
#define ICD_NUM_DEVICE_EXTENSIONS 6

static int icd_has_our_device_ext(const char *name) {
    for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) if (!strcmp(name, ICD_DEVICE_EXTENSIONS[i])) return 1;
    return 0;
}

/* The loader fetches a physical-device proc address through this and caches what it returns; if it
 * gets the HAL's copy it never sees the WSI extensions. So ours answers for the enumeration. */
static VkResult icd_enum_device_ext(void *pd, const char *layer, uint32_t *count, void *props);
static void *icd_gpdpa(void *pd, const char *pName) {
    if (!pName) return NULL;
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void *)icd_enum_device_ext;
    if (!strcmp(pName, "vkGetPhysicalDeviceFeatures"))
        return (void *)icd_get_phys_dev_features;
    if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2") || !strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR"))
        return (void *)icd_get_phys_dev_features2;
    if (!strcmp(pName, "vkGetPhysicalDeviceProperties2") || !strcmp(pName, "vkGetPhysicalDeviceProperties2KHR"))
        return (void *)icd_get_phys_dev_props2;
    if (!strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") || !strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (void *)icd_get_phys_dev_img_fmt_props2;
    if (!g_gipa && icd_load() != 0) return NULL;
    void *(*hal)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetPhysicalDeviceProcAddr");
    return hal ? hal(pd, pName) : NULL;
}

/* Device extensions this ICD does NOT pass through, because the HAL's support is broken and
 * advertising it makes apps take a path that faults. Measured: with VK_EXT_image_drm_format_modifier
 * visible, gamescope's Vulkan renderer imports the WSI's dma-buf as a
 * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image, and the HAL segfaults in vkAllocateMemory for the
 * import (vk_dmabuf_import_test.c, step 3); the same fd imports fine as a plain LINEAR image. Without
 * the extension the compositor falls back to the linear import, which works. */
static int icd_ext_suppressed(const char *name) {
    return !strcmp(name, "VK_EXT_image_drm_format_modifier");
}

static VkResult icd_enum_device_ext(void *pd, const char *layer, uint32_t *count, void *props) {
    if (pd) g_last_phys_dev = pd;
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*he)(void *, const char *, uint32_t *, void *) =
        (VkResult (*)(void *, const char *, uint32_t *, void *))g_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!he) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t hc = 0; he(pd, layer, &hc, NULL);
    void *tmp = NULL;
    struct { char name[256]; uint32_t ver; } *arr0 = NULL;
    if (hc) { tmp = malloc(hc * 260); he(pd, layer, &hc, tmp); arr0 = tmp; }
    /* The HAL's list minus the suppressed ones. */
    int add[ICD_NUM_DEVICE_EXTENSIONS], nadd = 0;
    if (hc) {
        for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) {
            int present = 0; for (uint32_t k = 0; k < hc; k++) if (!strcmp(arr0[k].name, ICD_DEVICE_EXTENSIONS[i])) { present = 1; break; }
            if (!present) add[nadd++] = i;
        }
    } else { for (int i = 0; i < ICD_NUM_DEVICE_EXTENSIONS; i++) add[nadd++] = i; }
    if (!props) {
        uint32_t keep = 0;
        for (uint32_t k = 0; k < hc; k++) if (!icd_ext_suppressed(arr0[k].name)) keep++;
        *count = keep + nadd; free(tmp); return VK_SUCCESS;
    }
    uint32_t room = *count, out = 0;
    uint32_t keep = 0;
    for (uint32_t k = 0; k < hc; k++) if (!icd_ext_suppressed(arr0[k].name)) keep++;
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    for (uint32_t k = 0; k < hc && out < room; k++) {
        if (icd_ext_suppressed(arr0[k].name)) continue;
        memcpy(arr[out].name, arr0[k].name, sizeof(arr[out].name));
        arr[out].ver = arr0[k].ver; out++;
    }
    for (int i = 0; i < nadd && out < room; i++) { strncpy(arr[out].name, ICD_DEVICE_EXTENSIONS[add[i]], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++; }
    uint32_t total = keep + nadd;
    *count = out; free(tmp);
    /* VK_INCOMPLETE means "more extensions exist than fit" - NOT "one was filtered out". Reporting it
     * when the list did fit makes the loader treat the enumeration as truncated and the app see the
     * extensions as missing (measured: gamescope reported VK_KHR_external_memory_fd et al. missing). */
    return (out < total) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Append the extensions this ICD provides to whatever the HAL reports, so the loader lets an app
 * enable VK_KHR_wayland_surface. Returns whether [name] is ours. */
static int icd_is_our_extension(const char *name) {
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS; i++)
        if (!strcmp(name, ICD_INSTANCE_EXTENSIONS[i])) return 1;
    return 0;
}

static VkResult icd_enum_instance_ext(const char *layer, uint32_t *count, void *props) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*he)(const char *, uint32_t *, void *) = (VkResult (*)(const char *, uint32_t *, void *))g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!he) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t hc = 0; he(NULL, &hc, NULL);
    if (!props) { *count = hc + ICD_NUM_INSTANCE_EXTENSIONS; return VK_SUCCESS; }
    uint32_t room = *count;
    uint32_t cap = room < hc ? room : hc;
    VkResult r = he(NULL, &cap, props);
    uint32_t out = cap;
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS && out < room; i++) {
        strncpy(arr[out].name, ICD_INSTANCE_EXTENSIONS[i], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++;
    }
    *count = out;
    return (r == VK_INCOMPLETE || cap < hc) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult icd_enum_instance_ext_OLD(const char *layer, uint32_t *count, void *props) {
    (void)layer; (void)count; (void)props;
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal_enum)(const char *, uint32_t *, void *) = (VkResult (*)(const char *, uint32_t *, void *))g_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!hal_enum) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    hal_enum(NULL, &n, NULL);
    if (!props) { *count = n + ICD_NUM_INSTANCE_EXTENSIONS; return VK_SUCCESS; }
    uint32_t room = *count;
    uint32_t cap = room < n ? room : n;
    VkResult r = hal_enum(NULL, &cap, props);
    uint32_t out = cap;
    struct { char name[256]; uint32_t ver; } *arr = (void *)props;
    for (int i = 0; i < ICD_NUM_INSTANCE_EXTENSIONS && out < room; i++) {
        strncpy(arr[out].name, ICD_INSTANCE_EXTENSIONS[i], 255); arr[out].name[255] = 0; arr[out].ver = 1; out++;
    }
    *count = out;
    return (r == VK_INCOMPLETE || cap < n) ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- VK_EXT_physical_device_drm -------------------------------------------------------------
 *
 * The PowerVR HAL advertises this extension and then writes NOTHING into
 * VkPhysicalDeviceDrmPropertiesEXT - measured on device: with the struct poisoned, every field is
 * still the poison after vkGetPhysicalDeviceProperties2 returns (see probes/pvr_drm_props_probe.c).
 * An app that zero-initialises the struct (the spec's advice) therefore reads hasPrimary = 0.
 *
 * gamescope 3.16.29 gates its backend on exactly that field (rendervulkan.cpp):
 *     if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary ) { "physical device has no
 *     primary node"; return false; }
 * and CWaylandBackend::UsesVulkanSwapchain() is false, so the nested Wayland backend was required to
 * have a KMS primary node it never opens. With the struct left empty the gate always fired, the
 * backend was deleted, and gamescope fell back to SDL against the instance it already made with an
 * empty extension list, which dies with "VK_KHR_wayland_surface extension is not enabled in the
 * Vulkan instance".
 *
 * The HAL's silence is what has to be corrected, so fill the struct in here from the node the HAL
 * actually drives. This is the answer a working DRM driver would give; the HAL omitting it is the
 * defect, not a fact to pass on. The gate is therefore satisfied truthfully rather than bypassed.
 *
 * hasPrimary comes out true on this device because it IS true: the PowerVR GPU exposes a card node
 * of its own. /sys/class/drm/card2 and /sys/class/drm/renderD128 are both symlinks into
 * /sys/devices/platform/34f00000.gpu0/drm/, and libdrm's drmGetDeviceFromDevId() reports them as one
 * device (probes/pvr_drm_gate_probe.c: "available_nodes: primary=/dev/dri/card2
 * render=/dev/dri/renderD128"). Nothing here invents a node: a device whose kernel driver registers
 * no primary would still report hasPrimary = 0, which is the correct answer for it.
 *
 * The entrypoint is reached through vk_icdGetInstanceProcAddr, which is where the loader resolves it
 * (its own trampoline calls the pointer the ICD returned there). Putting it in
 * vk_icdGetPhysicalDeviceProcAddr instead is dead code: the loader only consults that for entry
 * points it has no trampoline for, and vkGetPhysicalDeviceProperties2 has one. */
#define STYPE_PHYS_DEV_PROPERTIES_2 1000059000u
#define STYPE_DRM_PROPERTIES_EXT    1000353000u
#define STYPE_DRIVER_PROPERTIES_KHR 1000196000u
#define STYPE_PHYS_DEV_DRIVER_PROPERTIES 1000196000u
#define STYPE_PHYS_DEV_VULKAN_1_2_PROPERTIES 52u
#define VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA_ 25u

typedef struct {
    uint32_t sType; void *pNext;
    uint32_t hasPrimary, hasRender;
    int64_t primaryMajor, primaryMinor, renderMajor, renderMinor;
} PhysDevDrmProps;
typedef struct {
    uint32_t sType; void *pNext;
    uint32_t driverID;
    char driverName[256];
    char driverInfo[256];
    uint8_t conformanceVersion[4];
} PhysDevDriverProps;
typedef struct { uint32_t sType; void *pNext; void *properties; } PhysDevProps2;

static int g_drm_dev_known;
static int g_drm_has_primary;
static int64_t g_drm_primary_major, g_drm_primary_minor, g_drm_render_major, g_drm_render_minor;

/* Which DRM device a node belongs to, the way libdrm answers it: a card node and a render node are
 * the same device when the kernel put them in one device's sysfs directory, so their
 * /sys/dev/char/<maj>:<min>/device links resolve to the same target. Comparing device NUMBERS would
 * not work - card2 is 226:2 and renderD128 is 226:128, different numbers, one device. This phone's
 * card0 (display) and card1 (2D block) are separate DRM devices with their own directories, so they
 * correctly do not count as this GPU's primary. */
static int icd_node_device(const char *node, int64_t *maj, int64_t *min, char *device, size_t devlen) {
    struct stat st;
    char link[128], path[64];
    if (stat(node, &st) != 0) return 0;
    *maj = major(st.st_rdev); *min = minor(st.st_rdev);
    snprintf(path, sizeof(path), "/sys/dev/char/%lld:%lld/device", (long long)*maj, (long long)*min);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n <= 0) return 0;
    link[n] = 0;
    snprintf(device, devlen, "%s", link);
    return 1;
}

/* The primary node of a render node's DRM device: the card* entry of that device's own sysfs
 * device/drm/ directory. Being in that directory IS the membership test - the kernel puts a device's
 * nodes together - so no device-link comparison is needed.
 *
 * The numbers come from the entry's dev file rather than from stat()ing /dev/dri/card*, which cannot
 * work in the guest: its /dev/dri is a fabricated directory holding only the render node (a phone
 * app may not stat the real one - those nodes are graphics_device, which its policy does not grant),
 * so a card node is discoverable only through sysfs. */
static int icd_find_primary_for(const char *render_node, int64_t *pmaj, int64_t *pmin) {
    char device[128], dir[192];
    int64_t rmaj, rmin;
    if (!icd_node_device(render_node, &rmaj, &rmin, device, sizeof(device))) return 0;
    snprintf(dir, sizeof(dir), "/sys/dev/char/%lld:%lld/device/drm", (long long)rmaj, (long long)rmin);
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) && !found) {
        char devfile[256];
        if (strncmp(e->d_name, "card", 4) != 0) continue;
        snprintf(devfile, sizeof(devfile), "%s/%s/dev", dir, e->d_name);
        FILE *f = fopen(devfile, "r");
        if (!f) continue;
        long cmaj = -1, cmin = -1;
        if (fscanf(f, "%ld:%ld", &cmaj, &cmin) == 2 && cmaj >= 0 && cmin >= 0) {
            *pmaj = cmaj; *pmin = cmin; found = 1;
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

/* The DRM device behind the render node the HAL drives (PowerVR's is renderD128; nothing here
 * hardcodes that), and the primary node of that same device, if one is registered. */
static void icd_find_drm_dev(int *has_primary, int64_t *pmaj, int64_t *pmin, int64_t *rmaj, int64_t *rmin) {
    if (!g_drm_dev_known) {
        g_drm_dev_known = 1;
        for (int minor = 0; minor < 256; minor++) {
            char path[64];
            struct stat st;
            snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor);
            if (stat(path, &st) != 0) continue;
            g_drm_render_major = major(st.st_rdev);
            g_drm_render_minor = minor;
            if (icd_find_primary_for(path, &g_drm_primary_major, &g_drm_primary_minor)) g_drm_has_primary = 1;
            break;
        }
    }
    *has_primary = g_drm_has_primary;
    *pmaj = g_drm_primary_major; *pmin = g_drm_primary_minor;
    *rmaj = g_drm_render_major; *rmin = g_drm_render_minor;
}

/* The HAL refuses a DRM-format-modifier image query that also carries
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and the VkImageFormatListCreateInfo that goes with it, with
 * VK_ERROR_FEATURE_NOT_PRESENT:
 *
 *   SAMPLED|STORAGE|XFER_SRC                     -> VK_SUCCESS, EXPORTABLE|IMPORTABLE
 *   same + MUTABLE_FORMAT + format list           -> VK_ERROR_FEATURE_NOT_PRESENT
 *
 * gamescope asks the second way for its composited output: BInit sets that bit and list whenever an
 * image's UNORM and SRGB views differ, then treats any result other than VK_ERROR_FORMAT_NOT_SUPPORTED
 * as fatal, so it never reaches its own EXPORTABLE test and dies at "assert( modifiers.size() > 0 )".
 *
 * Mutable format describes a VIEW capability, not whether the format+modifier can be a dma-buf image
 * at all - which is what gamescope is really asking - so this retries that question without the two
 * bits, and only when the HAL's refusal is that specific error on a modifier-tiled query. */
#define VK_ERROR_FEATURE_NOT_PRESENT_ (-11)
#define VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT_ 1000158000u
#define VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_ 0x00000008u
#define STYPE_IMAGE_FORMAT_LIST 1000147000u
#define STYPE_EXT_IMG_FMT_INFO_ 1000071000u

typedef struct { uint32_t sType; void *pNext; uint32_t format, type, tiling, usage, flags; } ImgFmtInfo2;
typedef struct { uint32_t sType; void *pNext; } VkBase;
typedef struct { uint32_t sType; void *pNext; uint32_t viewFormatCount; const void *pViewFormats; } FmtList;

static void *icd_gipa_forward(void *instance, const char *name) {
    if (!g_gipa && icd_load() != 0) return NULL;
    return g_gipa ? g_gipa((VkInstance)instance, name) : NULL;
}

static VkResult icd_get_phys_dev_img_fmt_props2(void *pd, void *info, void *out) {
    if (pd) g_last_phys_dev = pd;
    VkResult (*hal)(void *, void *, void *) =
        (VkResult (*)(void *, void *, void *))icd_gipa_forward(g_instance, "vkGetPhysicalDeviceImageFormatProperties2");
    if (!hal) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = hal(pd, info, out);
    ImgFmtInfo2 *i = (ImgFmtInfo2 *)info;
    if (r != VK_ERROR_FEATURE_NOT_PRESENT_ || !i || i->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT_)
        return r;
    if (!(i->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_)) return r;

    /* Rebuild the pNext chain by UNLINKING the format-list struct from the live chain - each struct's
     * pNext is rewritten to skip it. (An array of pointers is not a pNext chain, and handing the HAL
     * one is what made an earlier attempt at this silently fail.) */
    ImgFmtInfo2 retry = *i;
    VkBase *prev = (VkBase *)&retry;
    for (VkBase *b = (VkBase *)i->pNext; b; b = (VkBase *)b->pNext) {
        if (b->sType == STYPE_IMAGE_FORMAT_LIST) { prev->pNext = b->pNext; continue; }
        prev->pNext = b; prev = b;
    }
    prev->pNext = NULL;
    retry.flags = i->flags & ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT_;

    VkResult r2 = hal(pd, &retry, out);
    if (icd_debug_on())
        fprintf(stderr, "pvr_icd: DRM-modifier query fmt=0x%x usage=0x%x with MUTABLE_FORMAT -> %d; "
                        "retried without it -> %d\n", i->format, i->usage, r, r2);
    return (r2 == VK_SUCCESS) ? r2 : r;   /* still unsupported: keep the original answer */
}

/* vkCreateImage: the HAL only binds imported dma-buf memory to LINEAR-tiling images. Measured
 * (probes/vk_dmabuf_import_test.c): importing a dma-buf into an OPTIMAL-tiling image - or a
 * VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT image - faults inside the HAL's vkAllocateMemory/vkBindImageMemory,
 * while the same fd binds fine to a LINEAR image. A compositor imports a client's dma-buf with
 * createFlags {bSampled,bTransferSrc} and neither bMappable nor bLinear, so its tiling is OPTIMAL
 * (gamescope rendervulkan.cpp:2102) - which is why gamescope logs "vkBindImageMemory failed".
 *
 * So an EXTERNAL dma-buf image is created LINEAR here, whatever tiling was asked for. This is safe for
 * a LINEAR buffer: the fd describes a linear layout, and an imported image's tiling only affects how
 * the GPU addresses it. Non-external images are untouched (their tiling is theirs to choose). */
#define STYPE_EXTERNAL_MEMORY_IMAGE_CREATE 1000072001u
#define VK_IMAGE_TILING_OPTIMAL_ 0u
#define VK_IMAGE_TILING_LINEAR_ 1u
typedef struct { uint32_t sType; void *pNext; uint32_t handleTypes; } ExtMemImgCI;
typedef struct { uint32_t sType; void *pNext; uint32_t flags; uint32_t imageType; uint32_t format;
                 uint32_t width, height, depth; uint32_t mipLevels, arrayLayers; uint32_t samples;
                 uint32_t tiling; uint32_t usage; uint32_t sharingMode; uint32_t queueFamilyIndexCount;
                 const uint32_t *pQueueFamilyIndices; uint32_t initialLayout; } ImgCI;
static VkResult icd_create_image(void *device, const void *ci, const void *alloc, void **out) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    /* Fetch the HAL's own vkCreateImage through the device's gdpa, or the pointer would be this
     * function again (gipa returns the ICD's entries). */
    void *(*dgdpa)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(device, "vkGetDeviceProcAddr");
    VkResult (*hal)(void *, const void *, const void *, void **) =
        dgdpa ? (VkResult (*)(void *, const void *, const void *, void **))dgdpa(device, "vkCreateImage") : NULL;
    if (!hal || hal == icd_create_image) {
        void *(*igdpa)(void *, const char *) = (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
        if (igdpa) hal = (VkResult (*)(void *, const void *, const void *, void **))igdpa(device, "vkCreateImage");
    }
    if (!hal || hal == icd_create_image) return VK_ERROR_INITIALIZATION_FAILED;
    const ImgCI *in = (const ImgCI *)ci;
    int external = 0;
    for (const VkBase *b = (const VkBase *)in->pNext; b; b = (const VkBase *)b->pNext)
        if (b->sType == STYPE_EXTERNAL_MEMORY_IMAGE_CREATE) { external = 1; break; }
    { FILE *o = diagf(); if (o) { fprintf(o, "ICD createImage pid=%d tiling=%u external=%d fmt=0x%x usage=0x%x\n",
        (int)getpid(), in->tiling, external, in->format, in->usage); fflush(o); } }
    /* PVR_ICD_FORCE_LINEAR=1: create every image of this process LINEAR. Gamescope picks its
     * wayland-backend output texture's tiling itself - rendervulkan.cpp:2102,
     * `tiling = (bMappable || bLinear) ? LINEAR : OPTIMAL` - and creates it without an
     * external-memory chain, so the rewrite below never sees it. It then exports an OPTIMAL
     * dma-buf: measured 2026-11-13, 1602x720 at stride 8192 exports 4,837,376 bytes where the linear
     * layout needs 5,898,240 (82 %, exactly 1181 pages), and no linear import can bind that - the
     * compositor's vkBindImageMemory answers OUT_OF_HOST_MEMORY. Gamescope is a prebuilt binary here
     * (its build needs an Arch arm64 container), so the tiling it would have chosen is overridden in
     * the ICD instead: with every image LINEAR, what it exports is the linear layout this compositor
     * binds, which is the configuration that already displays for glxgears. */
    static int force_linear = -1;
    if (force_linear < 0) {
        const char *v = getenv("PVR_ICD_FORCE_LINEAR");
        if (v) {
            force_linear = (*v && *v != '0') ? 1 : 0;      /* explicit wins, including "0" */
        } else {
            /* Default: gamescope only. It is the producer whose export reaches the compositor; games
             * and Steam keep whatever tiling they ask for, since a linear render target costs fill
             * rate on this GPU. Scoped by name rather than by fd or usage because the client that
             * exports is exactly the process running the compositor. */
            char comm[32] = {0};
            FILE *c = fopen("/proc/self/comm", "r");
            if (c) {
                if (fgets(comm, sizeof comm, c)) {
                    size_t n = strlen(comm);
                    while (n && (comm[n - 1] == '\n' || comm[n - 1] == '\r')) comm[--n] = 0;
                }
                fclose(c);
            }
            force_linear = strcmp(comm, "gamescope") == 0;
        }
    }
    static int ext_logged = 0, err_logged = 0;
    VkResult r;
    if (force_linear) {
        static int forced_logged, lin_logged;
        if (in->tiling == VK_IMAGE_TILING_LINEAR_) {
            if (lin_logged < 5) { lin_logged++;
                fprintf(stderr, "pvr_icd: FORCE_LINEAR pid=%d image already LINEAR fmt=0x%x usage=0x%x %ux%u\n",
                        (int)getpid(), in->format, in->usage, in->width, in->height); }
        } else if (!external) {
            ImgCI copy = *in; copy.tiling = VK_IMAGE_TILING_LINEAR_;
            if (forced_logged < 5) { forced_logged++;
                fprintf(stderr, "pvr_icd: FORCE_LINEAR pid=%d tiling %u -> LINEAR fmt=0x%x usage=0x%x %ux%u\n",
                        (int)getpid(), in->tiling, in->format, in->usage, in->width, in->height); }
            r = hal(device, &copy, alloc, out);
            if (r != VK_SUCCESS && err_logged < 64) {
                err_logged++;
                icd_wsi_logf("vkCreateImage(FORCE_LINEAR) FAILED fmt=%u %ux%u tiling=LINEAR(was %u) usage=0x%x flags=0x%x ext=%d -> rc=%d",
                             in->format, in->width, in->height, in->tiling, in->usage, in->flags, external, r);
            }
            return r;
        }
    }
    if (external && in->tiling != VK_IMAGE_TILING_LINEAR_) {
        ImgCI copy = *in; copy.tiling = VK_IMAGE_TILING_LINEAR_;
        if (icd_debug_on())
            fprintf(stderr, "pvr_icd: external image tiling %u -> LINEAR (the HAL imports only LINEAR)\n", in->tiling);
        r = hal(device, &copy, alloc, out);
        if ((r != VK_SUCCESS || ext_logged < 64) && err_logged < 64) {
            if (r == VK_SUCCESS) ext_logged++; else err_logged++;
            icd_wsi_logf("vkCreateImage(ext->LINEAR) fmt=%u %ux%u tiling=%u->LINEAR usage=0x%x flags=0x%x ext=1 -> rc=%d",
                         in->format, in->width, in->height, in->tiling, in->usage, in->flags, r);
        }
        if (r != VK_SUCCESS) {
            VkResult r2 = hal(device, ci, alloc, out);
            if (err_logged < 64) {
                err_logged++;
                icd_wsi_logf("vkCreateImage(ext fallback orig tiling=%u) fmt=%u %ux%u usage=0x%x flags=0x%x -> rc=%d",
                             in->tiling, in->format, in->width, in->height, in->usage, in->flags, r2);
            }
            return r2;
        }
        return r;
    }
    r = hal(device, ci, alloc, out);
    if ((r != VK_SUCCESS && err_logged < 64) || (external && ext_logged < 64)) {
        if (r != VK_SUCCESS) err_logged++;
        else ext_logged++;
        icd_wsi_logf("vkCreateImage fmt=%u %ux%u tiling=%u usage=0x%x flags=0x%x ext=%d -> rc=%d",
                     in->format, in->width, in->height, in->tiling, in->usage, in->flags, external, r);
    }
    return r;
}

/* Intercept vkCreateShaderModule to log and save SPIR-V bytecode. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    size_t codeSize; const uint32_t *pCode;
} ShaderModuleCI;

static VkResult icd_create_shader_module(void *device, const void *ci, const void *alloc, void **out) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    void *(*dgdpa)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(device, "vkGetDeviceProcAddr");
    VkResult (*hal)(void *, const void *, const void *, void **) =
        dgdpa ? (VkResult (*)(void *, const void *, const void *, void **))dgdpa(device, "vkCreateShaderModule") : NULL;
    if (!hal || hal == icd_create_shader_module) {
        void *(*igdpa)(void *, const char *) = (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
        if (igdpa) hal = (VkResult (*)(void *, const void *, const void *, void **))igdpa(device, "vkCreateShaderModule");
    }
    if (!hal || hal == icd_create_shader_module) return VK_ERROR_INITIALIZATION_FAILED;

    const ShaderModuleCI *in = (const ShaderModuleCI *)ci;
    VkResult r = hal(device, ci, alloc, out);
    if (r != VK_SUCCESS || icd_debug_on()) {
        fprintf(stderr, "pvr_icd: vkCreateShaderModule codeSize=%zu -> mod=%p rc=%d\n",
                in ? in->codeSize : 0, (out && *out) ? *out : NULL, r);
    }
    return r;
}

/* Intercept vkCreateComputePipelines to log and fix specialization constants.
 * Gamescope maps its composite specialization constants to IDs 0..6, but c_itm_enable
 * in descriptor_set.h has constant_id = 7 (constant 6 does not exist in the shader).
 * On PowerVR, invalid specialization constants make vkCreateComputePipelines fail with -13. */
typedef struct {
    uint32_t constantID; uint32_t offset; size_t size;
} SpecMapEntry;

typedef struct {
    uint32_t mapEntryCount; const SpecMapEntry *pMapEntries; size_t dataSize; const void *pData;
} SpecInfo;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags; uint32_t stage;
    void *module; const char *pName; const SpecInfo *pSpecializationInfo;
} PipeShaderStageCI;

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    PipeShaderStageCI stage; void *layout; void *basePipelineHandle; int32_t basePipelineIndex;
} CompPipeCI;

static VkResult icd_create_compute_pipelines(void *device, void *pipelineCache, uint32_t count,
                                             const void *pCreateInfos, const void *alloc, void **pPipelines) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    void *(*dgdpa)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(device, "vkGetDeviceProcAddr");
    VkResult (*hal)(void *, void *, uint32_t, const void *, const void *, void **) =
        dgdpa ? (VkResult (*)(void *, void *, uint32_t, const void *, const void *, void **))dgdpa(device, "vkCreateComputePipelines") : NULL;
    if (!hal || hal == icd_create_compute_pipelines) {
        void *(*igdpa)(void *, const char *) = (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
        if (igdpa) hal = (VkResult (*)(void *, void *, uint32_t, const void *, const void *, void **))igdpa(device, "vkCreateComputePipelines");
    }
    if (!hal || hal == icd_create_compute_pipelines) return VK_ERROR_INITIALIZATION_FAILED;

    const CompPipeCI *cis = (const CompPipeCI *)pCreateInfos;
    if (icd_debug_on()) {
        for (uint32_t i = 0; i < count; i++) {
            fprintf(stderr, "pvr_icd: vkCreateComputePipelines[%u]: mod=%p name=%s layout=%p spec=%p\n",
                    i, cis[i].stage.module, cis[i].stage.pName ? cis[i].stage.pName : "(null)",
                    cis[i].layout, cis[i].stage.pSpecializationInfo);
            if (cis[i].stage.pSpecializationInfo) {
                const SpecInfo *si = cis[i].stage.pSpecializationInfo;
                fprintf(stderr, "pvr_icd:   spec mapCount=%u dataSz=%zu\n", si->mapEntryCount, si->dataSize);
                for (uint32_t k = 0; k < si->mapEntryCount; k++) {
                    uint32_t val = 0;
                    if (si->pData && si->pMapEntries[k].offset + sizeof(uint32_t) <= si->dataSize)
                        memcpy(&val, (const char *)si->pData + si->pMapEntries[k].offset, sizeof(uint32_t));
                    fprintf(stderr, "pvr_icd:     [%u] id=%u off=%u sz=%zu val=%u\n",
                            k, si->pMapEntries[k].constantID, si->pMapEntries[k].offset, si->pMapEntries[k].size, val);
                }
            }
        }
    }

    VkResult r = hal(device, pipelineCache, count, pCreateInfos, alloc, pPipelines);
    if (r != VK_SUCCESS || icd_debug_on())
        fprintf(stderr, "pvr_icd: HAL vkCreateComputePipelines count=%u returned rc=%d\n", count, r);
    if (r == VK_SUCCESS) return r;

    /* Fallback logic if the HAL failed with VK_ERROR_UNKNOWN (-13) or other error */
    VkResult final_r = VK_SUCCESS;
    for (uint32_t i = 0; i < count; i++) {
        CompPipeCI ci = cis[i];
        VkResult r_pipe = VK_ERROR_UNKNOWN;

        if (ci.stage.pSpecializationInfo) {
            /* Fallback 1: gamescope bug: c_itm_enable is SpecId 7, not 6 */
            SpecInfo mod_si = *ci.stage.pSpecializationInfo;
            SpecMapEntry entries[16];
            uint32_t n = 0;
            for (uint32_t k = 0; k < mod_si.mapEntryCount && k < 16; k++) {
                SpecMapEntry e = mod_si.pMapEntries[k];
                if (e.constantID == 6) e.constantID = 7;
                entries[n++] = e;
            }
            mod_si.mapEntryCount = n;
            mod_si.pMapEntries = entries;
            ci.stage.pSpecializationInfo = &mod_si;
            r_pipe = hal(device, pipelineCache, 1, &ci, alloc, &pPipelines[i]);
            fprintf(stderr, "pvr_icd: fallback 1 (constID 6->7) pipe[%u] rc=%d\n", i, r_pipe);
            if (r_pipe == VK_SUCCESS) continue;

            /* Fallback 2: omit constantID 6 entirely (pass only declared constants 0..5) */
            n = 0;
            for (uint32_t k = 0; k < ci.stage.pSpecializationInfo->mapEntryCount && k < 16; k++) {
                if (ci.stage.pSpecializationInfo->pMapEntries[k].constantID == 6) continue;
                entries[n++] = ci.stage.pSpecializationInfo->pMapEntries[k];
            }
            mod_si.mapEntryCount = n;
            mod_si.pMapEntries = entries;
            ci.stage.pSpecializationInfo = &mod_si;
            r_pipe = hal(device, pipelineCache, 1, &ci, alloc, &pPipelines[i]);
            fprintf(stderr, "pvr_icd: fallback 2 (omit constID 6) pipe[%u] rc=%d\n", i, r_pipe);
            if (r_pipe == VK_SUCCESS) continue;

            /* Fallback 3: specialization info NULL (use shader module defaults) */
            ci.stage.pSpecializationInfo = NULL;
            r_pipe = hal(device, pipelineCache, 1, &ci, alloc, &pPipelines[i]);
            fprintf(stderr, "pvr_icd: fallback 3 (pSpecializationInfo=NULL) pipe[%u] rc=%d\n", i, r_pipe);
            if (r_pipe == VK_SUCCESS) continue;
        }

        if (r_pipe != VK_SUCCESS) final_r = r_pipe;
    }
    return final_r;
}

typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags;
    uint32_t stageCount; const PipeShaderStageCI *pStages;
} GraphPipeCIHdr;

static VkResult icd_create_graphics_pipelines(void *device, void *pipelineCache, uint32_t count,
                                              const void *pCreateInfos, const void *alloc, void **pPipelines) {
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    void *(*dgdpa)(void *, const char *) =
        (void *(*)(void *, const char *))g_gipa(device, "vkGetDeviceProcAddr");
    VkResult (*hal)(void *, void *, uint32_t, const void *, const void *, void **) =
        dgdpa ? (VkResult (*)(void *, void *, uint32_t, const void *, const void *, void **))dgdpa(device, "vkCreateGraphicsPipelines") : NULL;
    if (!hal || hal == icd_create_graphics_pipelines) {
        void *(*igdpa)(void *, const char *) = (void *(*)(void *, const char *))g_gipa(g_instance, "vkGetDeviceProcAddr");
        if (igdpa) hal = (VkResult (*)(void *, void *, uint32_t, const void *, const void *, void **))igdpa(device, "vkCreateGraphicsPipelines");
    }
    if (!hal || hal == icd_create_graphics_pipelines) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = hal(device, pipelineCache, count, pCreateInfos, alloc, pPipelines);
    static int gp_logged = 0, gp_err = 0;
    if (r != VK_SUCCESS || (icd_debug_on() && gp_logged < 16)) {
        if (r != VK_SUCCESS) gp_err++;
        else gp_logged++;
        if (r != VK_SUCCESS ? (gp_err <= 64) : 1) {
            const GraphPipeCIHdr *hdr = (count > 0 && pCreateInfos) ? (const GraphPipeCIHdr *)pCreateInfos : NULL;
            icd_wsi_logf("vkCreateGraphicsPipelines count=%u cache=%p stage0Count=%u -> rc=%d",
                         count, pipelineCache, hdr ? hdr->stageCount : 0u, r);
            if (r != VK_SUCCESS && hdr && hdr->pStages && hdr->stageCount <= 16) {
                for (uint32_t k = 0; k < hdr->stageCount; k++) {
                    icd_wsi_logf("  stage[%u] flags=0x%x stage=0x%x mod=%p name=%s spec=%p",
                                 k, hdr->pStages[k].flags, hdr->pStages[k].stage, hdr->pStages[k].module,
                                 hdr->pStages[k].pName ? hdr->pStages[k].pName : "(null)",
                                 (const void *)hdr->pStages[k].pSpecializationInfo);
                }
            }
        }
    }
    return r;
}

static void icd_get_phys_dev_props2(void *pd, void *props2) {
    if (pd) g_last_phys_dev = pd;
    PhysDevProps2 *p2 = (PhysDevProps2 *)props2;
    if (!p2) return;
    /* The HAL answers for the rest of the chain; it just leaves this struct empty. */
    if (!g_gipa && icd_load() != 0) return;
    void (*hal)(void *, void *) = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(NULL, "vkGetPhysicalDeviceProperties2");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(NULL, "vkGetPhysicalDeviceProperties2KHR");
    if (hal) hal(pd, props2);
    int has_primary; int64_t pmaj, pmin, rmaj, rmin;
    icd_find_drm_dev(&has_primary, &pmaj, &pmin, &rmaj, &rmin);
    for (void *node = p2->pNext; node; node = ((void **)node)[1]) {
        uint32_t sType = *(uint32_t *)node;
        if (sType == STYPE_DRM_PROPERTIES_EXT) {
            PhysDevDrmProps *d = (PhysDevDrmProps *)node;
            d->hasPrimary = (uint32_t)has_primary;
            d->primaryMajor = pmaj; d->primaryMinor = pmin;
            d->hasRender = 1;
            d->renderMajor = rmaj; d->renderMinor = rmin;
            if (icd_debug_on())
                fprintf(stderr, "pvr_icd: physical_device_drm: hasPrimary=%d primary=%lld:%lld render=%lld:%lld\n",
                        has_primary, (long long)pmaj, (long long)pmin, (long long)rmaj, (long long)rmin);
        } else if (sType == STYPE_DRIVER_PROPERTIES_KHR || sType == STYPE_PHYS_DEV_DRIVER_PROPERTIES || sType == STYPE_PHYS_DEV_VULKAN_1_2_PROPERTIES) {
            PhysDevDriverProps *dp = (PhysDevDriverProps *)node;
            static int drv_logged = 0;
            if (!drv_logged || icd_debug_on()) {
                drv_logged = 1;
                fprintf(stderr, "pvr_icd: driver_properties (sType=%u): driverID was %u, overriding to %u\n",
                        sType, dp->driverID, VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA_);
                fflush(stderr);
            }
            dp->driverID = VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA_;
        } else if (sType == 1000272001u /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT */) {
            typedef struct { uint32_t sType; uint32_t pad; void *pNext; uint64_t minPlacedMemoryMapAlignment; } PlacedProps;
            ((PlacedProps *)node)->minPlacedMemoryMapAlignment = 4096;
        }
    }
}

/* PowerVR DXT natively supports BC1..BC5 (optimalTilingFeatures=0xd401) but omits BC6H/BC7, so
 * the HAL reports textureCompressionBC=0. Advertising it lets DXVK and Zink use hardware BC1..BC5
 * instead of skipping the GPU or software-decompressing DXT textures on the CPU. */
static void icd_get_phys_dev_features(void *pd, void *features) {
    if (!features) return;
    if (!g_gipa && icd_load() != 0) return;
    void (*hal)(void *, void *) = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceFeatures");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(NULL, "vkGetPhysicalDeviceFeatures");
    if (hal) {
        hal(pd, features);
        ((uint32_t *)features)[22] = 1; /* textureCompressionBC */
    }
}

static void icd_get_phys_dev_features2(void *pd, void *features2) {
    if (!features2) return;
    if (!g_gipa && icd_load() != 0) return;
    void (*hal)(void *, void *) = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(NULL, "vkGetPhysicalDeviceFeatures2");
    if (!hal) hal = (void (*)(void *, void *))g_gipa(NULL, "vkGetPhysicalDeviceFeatures2KHR");
    if (hal) {
        hal(pd, features2);
        ((uint32_t *)((uint8_t *)features2 + 16))[22] = 1; /* features.textureCompressionBC */
    } else {
        icd_get_phys_dev_features(pd, (uint8_t *)features2 + 16);
    }
    for (void *node = ((void **)features2)[1]; node; node = ((void **)node)[1]) {
        if (*(uint32_t *)node == 1000272000u /* VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT */) {
            typedef struct { uint32_t sType; uint32_t pad; void *pNext; uint32_t placed; uint32_t rangePlaced; uint32_t unmapReserve; } PlacedFeats;
            PlacedFeats *pf = (PlacedFeats *)node;
            pf->placed = 1;
            pf->rangePlaced = 1;
            pf->unmapReserve = 1;
        }
    }
}

/* Intercept instance creation: make one for the HAL without our extension names in it. */
typedef struct {
    uint32_t sType; const void *pNext; uint32_t flags; const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} InstanceCreateInfo;
static VkResult icd_create_instance(const void *ci, const void *alloc, VkInstance *out) {
    icd_install_signal_handlers();
    icd_install_x11_handlers("vkCreateInstance");
    if (!g_gipa && icd_load() != 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult (*hal_create)(const void *, const void *, VkInstance *) =
        (VkResult (*)(const void *, const void *, VkInstance *))g_gipa(NULL, "vkCreateInstance");
    if (!hal_create) return VK_ERROR_INITIALIZATION_FAILED;
    const InstanceCreateInfo *in = (const InstanceCreateInfo *)ci;
    uint32_t n = in->enabledExtensionCount;
    const char *const *names = in->ppEnabledExtensionNames;
    VkInstance created = NULL;   /* only published once the HAL returned success */
    VkResult r;
    if (n == 0) {
        r = hal_create(ci, alloc, &created);
    } else {
        const char **kept = (const char **)malloc(sizeof(char *) * n);
        uint32_t kn = 0;
        for (uint32_t i = 0; i < n; i++) if (!icd_is_our_extension(names[i])) kept[kn++] = names[i];
        InstanceCreateInfo copy = *in;
        copy.enabledExtensionCount = kn;
        copy.ppEnabledExtensionNames = kept;
        r = hal_create(&copy, alloc, &created);
        free(kept);
    }
    if (r != VK_SUCCESS) return r;
    /* g_instance is the handle device procs are resolved against; a failed create must not leave a
     * stale one, and *out must not be handed a garbage handle. */
    g_instance = created;
    if (out) *out = created;
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
int vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *version) {
    /* We implement up to loader interface 5; the loader caps to what we report. */
    if (version && *version > 5) *version = 5;
    icd_load();
    return VK_SUCCESS;
}

__attribute__((visibility("default")))
void *vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    void *wsi = pvr_wsi_proc(pName);
    if (wsi) return wsi;
    void *wsi_sync = pvr_wsi_sync_proc(pName);
    if (wsi_sync) return wsi_sync;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (void *)icd_gdpa;
    if (!strcmp(pName, "vkGetPhysicalDeviceProcAddr")) return (void *)icd_gpdpa;
    if (!strcmp(pName, "vkCreateDevice")) return (void *)icd_create_device;
    if (!strcmp(pName, "vkEnumerateDeviceExtensionProperties")) return (void *)icd_enum_device_ext;
    if (!strcmp(pName, "vkEnumerateInstanceExtensionProperties")) return (void *)icd_enum_instance_ext;
    if (!strcmp(pName, "vkCreateInstance")) return (void *)icd_create_instance;
    /* Core device function the loader fetches through GIPA to fill its device dispatch - intercept
     * here too, or the external-image tiling fix (icd_create_image) is bypassed. */
    if (!strcmp(pName, "vkCreateImage")) return (void *)icd_create_image;
    if (!strcmp(pName, "vkCreateShaderModule")) return (void *)icd_create_shader_module;
    if (!strcmp(pName, "vkCreateComputePipelines")) return (void *)icd_create_compute_pipelines;
    if (!strcmp(pName, "vkCreateGraphicsPipelines")) return (void *)icd_create_graphics_pipelines;
    if (!strcmp(pName, "vkAllocateMemory")) return (void *)icd_allocate_memory;
    if (!strcmp(pName, "vkFreeMemory")) return (void *)icd_free_memory;
    if (!strcmp(pName, "vkMapMemory")) return (void *)icd_map_memory;
    if (!strcmp(pName, "vkUnmapMemory")) return (void *)icd_unmap_memory;
    if (!strcmp(pName, "vkMapMemory2") || !strcmp(pName, "vkMapMemory2KHR")) return (void *)icd_map_memory2;
    if (!strcmp(pName, "vkUnmapMemory2") || !strcmp(pName, "vkUnmapMemory2KHR")) return (void *)icd_unmap_memory2;
    /* The loader fills its device dispatch from these, so this is the path a command actually takes
     * (hooking icd_gdpa alone saw nothing: zero pvr_cmd lines). */
    { void *cmd = pvr_icd_cmd_proc(NULL, pName); if (cmd) return cmd; }
    /* The loader's own trampoline for vkGetPhysicalDeviceProperties2 calls whatever this returns, so
     * this is where the HAL's empty VK_EXT_physical_device_drm answer is corrected. */
    if (!strcmp(pName, "vkGetPhysicalDeviceFeatures"))
        return (void *)icd_get_phys_dev_features;
    if (!strcmp(pName, "vkGetPhysicalDeviceFeatures2") || !strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR"))
        return (void *)icd_get_phys_dev_features2;
    if (!strcmp(pName, "vkGetPhysicalDeviceProperties2") || !strcmp(pName, "vkGetPhysicalDeviceProperties2KHR"))
        return (void *)icd_get_phys_dev_props2;
    /* A DRM-modifier image query the HAL refuses only because of the mutable-format bits; see the
     * note above icd_get_phys_dev_img_fmt_props2. */
    if (!strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2") ||
        !strcmp(pName, "vkGetPhysicalDeviceImageFormatProperties2KHR"))
        return (void *)icd_get_phys_dev_img_fmt_props2;
    if (!g_gipa) { if (icd_load() != 0) return NULL; }
    void *r = g_gipa(instance, pName);
    /* Diagnostic: which instance entry points fall through to the HAL and, of those, which it does
     * not answer - a NULL becomes a NULL in the app's dispatch table. Off unless PVR_ICD_DIAG. */
    FILE *o = diagf();
    if (o) { fprintf(o, "ICD gipa pid=%d name=%s -> %p\n", (int)getpid(), pName, r); fflush(o); }
    return r;
}

__attribute__((visibility("default")))
void *vk_icdGetPhysicalDeviceProcAddr(VkPhysicalDevice pd, const char *pName) {
    return icd_gpdpa((void *)pd, pName);
}

/* A loader that implements ICD interface 1 (and every loader from 1.3.x on) calls vk_icdCreateInstance
 * DIRECTLY rather than fetching "vkCreateInstance" from vk_icdGetInstanceProcAddr. This used to hand
 * the app's create-info straight to the HAL, which meant VK_KHR_wayland_surface / VK_KHR_xcb_surface /
 * VK_KHR_xlib_surface reached the PowerVR blob -- it knows only Android surfaces, so the call failed and
 * SDL reported "VK_KHR_wayland_surface extension is not enabled in the Vulkan instance" and could not
 * create its backend, which is what stopped the Steam session. Route it through icd_create_instance,
 * which strips our extension names before the HAL sees them; the app still gets them on the instance
 * this ICD serves. */
__attribute__((visibility("default")))
int vk_icdCreateInstance(const void *ci, const void *alloc, VkInstance *out) {
    return (int)icd_create_instance(ci, alloc, out);
}
