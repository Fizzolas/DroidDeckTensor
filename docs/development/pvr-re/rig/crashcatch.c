/* crashcatch - write a report when a process takes a fatal signal, even though it installs its own
 * handler.
 *
 * The Steam client calls breakpad's Init about a second in, which does sigaction(SIGSEGV, ...) and
 * replaces any handler an LD_PRELOAD constructor installed - so a plain catcher reports nothing. This
 * interposes sigaction() itself: when the process asks for a handler on a fatal signal, the request
 * is remembered and this wrapper is installed in its place. The wrapper writes the report and then
 * re-raises with the original disposition, so the process still dies the way it would have.
 *
 * Writes signal, faulting address, registers, a raw stack scan (binaries here are built without frame
 * pointers, so return addresses are found in the stack words rather than by walking a chain) and
 * /proc/self/maps - one section at a time, fsync'd, so a second fault mid-report cannot lose what is
 * already on disk.
 *
 * The handler is installed only for the process whose comm is "steam" (CRASHCATCH_ALL=1 overrides):
 * several of Steam's helpers take faults on purpose and recover, and catching one of those turns a
 * recovered fault into a kill.
 *
 * Build: build.sh. Output: /tmp/crash.txt.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <stdio.h>

#define OUT "/tmp/crash.txt"
#define STACK_WORDS 900

static volatile sig_atomic_t entered;
static int fd = -1;

/* The process's own handler for each signal we wrap, so it still runs after the report. */
static struct sigaction app_act[NSIG];
static volatile sig_atomic_t have_app[NSIG];

static void w(const char *s) { if (fd >= 0) { ssize_t r = write(fd, s, strlen(s)); (void)r; } }
static void wc(int c) { if (fd >= 0) { char b = (char)c; ssize_t r = write(fd, &b, 1); (void)r; } }
static void wx(unsigned long long v) {
    char t[19]; int i = 18; t[i] = 0;
    if (!v) t[--i] = '0';
    while (v) { t[--i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    w("0x"); if (fd >= 0) { ssize_t r = write(fd, t + i, (size_t)(18 - i)); (void)r; }
}
static void wd(long long v) {
    char t[24]; int i = 23; t[i] = 0;
    int neg = v < 0;
    unsigned long long u = neg ? -(unsigned long long)v : (unsigned long long)v;
    if (!u) t[--i] = '0';
    while (u) { t[--i] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) t[--i] = '-';
    w(t + i);
}
static void fsync_out(void) { if (fd >= 0) fsync(fd); }

static void handler(int sig, siginfo_t *si, void *ctx) {
    if (!entered) {
        entered = 1;
        fd = open(OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ucontext_t *uc = (ucontext_t *)ctx;
            mcontext_t *m = uc ? &uc->uc_mcontext : NULL;
            w("comm=");
            { int cf = open("/proc/self/comm", O_RDONLY); if (cf >= 0) { char cb[64]; ssize_t cn = read(cf, cb, sizeof(cb)); close(cf);
                if (cn > 0) { if (cb[cn-1] == 10) cn--; ssize_t r = write(fd, cb, (size_t)cn); (void)r; } } }
            w("  caught signal "); wd(sig);
            w("  si_addr="); wx((unsigned long long)(uintptr_t)(si ? si->si_addr : 0));
            w("  tid="); wd((long long)syscall(SYS_gettid));
            w("  pid="); wd((long long)getpid());
            wc(10);
            if (m) {
                w("pc=");  wx(m->pc);
                w("  lr(x30)="); wx(m->regs[30]);
                w("  sp="); wx(m->sp);
                w("  fp(x29)="); wx(m->regs[29]);
                wc(10);
                for (int i = 0; i <= 30; i++) {
                    if (i % 4 == 0) { wc(10); w("x"); wd(i); w(": "); }
                    wx(m->regs[i]); wc(32);
                }
                wc(10);
            }
            fsync_out();
            if (m && m->sp) {
                unsigned long long *sp = (unsigned long long *)(uintptr_t)m->sp;
                w("--- stack scan from sp (executable-mapping words are return addresses) ---");
                wc(10);
                for (int i = 0; i < STACK_WORDS; i++) {
                    unsigned long long v;
                    memcpy(&v, &sp[i], sizeof(v));
                    if (i % 4 == 0) { w("sp+"); wd((long long)(i * 8)); w(": "); }
                    wx(v); wc(32);
                }
                wc(10);
                fsync_out();
            }
            /* Where each module lives, from maps rather than from an offset into a fixed one: which
             * module a fault is in changes as the PowerVR faults get fixed, and reading a base as
             * "pc minus some constant" gives a plausible wrong number the moment it does. The
             * constants below are file offsets within each module and are only applied once the
             * module is known from maps. */
            unsigned long long lb_base = 0, hal_base = 0, icd_base = 0;
            {
                int mf = open("/proc/self/maps", O_RDONLY);
                if (mf >= 0) {
                    static char mb[1 << 19];
                    ssize_t n = read(mf, mb, sizeof(mb) - 1);
                    close(mf);
                    if (n > 0) {
                        mb[n] = 0;
                        for (char *q = mb; q && *q; ) {
                            char *nl = strchr(q, 10);
                            if (nl) *nl = 0;
                            unsigned long long lo, off;
                            char perms[8] = {0};
                            if (sscanf(q, "%llx-%*x %7s %llx", &lo, perms, &off) == 3 && off == 0) {
                                if (strstr(q, "/powervr-g5x/libhybris.so")) lb_base = lo;
                                else if (strstr(q, "/hw/vulkan.powervr.so")) hal_base = lo;
                                else if (strstr(q, "/powervr-g5x/libvulkan_powervr.so")) icd_base = lo;
                            }
                            q = nl ? nl + 1 : NULL;
                        }
                    }
                }
            }
            w("libhybris_base="); wx(lb_base);
            w("  hal_base="); wx(hal_base);
            w("  icd_base="); wx(icd_base);
            wc(10);
            /* q.so's guard counter, only meaningful when q.so is the module in question. */
            if (m && lb_base) {
                /* q.so is a sibling of libhybris.so; find it too, offset 0xeeea8 into its image. */
                unsigned long long qb = 0;
                int mf = open("/proc/self/maps", O_RDONLY);
                if (mf >= 0) {
                    static char qb2[1 << 19];
                    ssize_t n = read(mf, qb2, sizeof(qb2) - 1);
                    close(mf);
                    if (n > 0) { qb2[n] = 0;
                        for (char *q = qb2; q && *q; ) {
                            char *nl = strchr(q, 10); if (nl) *nl = 0;
                            unsigned long long lo, off;
                            if (sscanf(q, "%llx-%*x %*s %llx", &lo, &off) == 2 && off == 0 &&
                                strstr(q, "/powervr-g5x/q.so")) { qb = lo; break; }
                            q = nl ? nl + 1 : NULL;
                        }
                    }
                }
                if (qb) {
                    volatile int *rc = (volatile int *)(uintptr_t)(qb + 0xeeea8);
                    w("q.so_base="); wx(qb); w("  ref_count_="); wd(*rc); wc(10);
                }
            }
            /* The HAL's PLT, for the "was it ever resolved" question. Its base is from maps, not
             * from pc, so this stays correct whichever module the fault moves into. */
            if (hal_base) {
                unsigned long long *got = (unsigned long long *)(uintptr_t)(hal_base + 0x286ab0);
                w("HAL .got.plt slots (index:value):"); wc(10);
                for (int i = 0; i < 352; i++) {
                    unsigned long long v;
                    memcpy(&v, &got[i], sizeof(v));
                    wd(i); w(":"); wx(v); wc(32);
                    if (i % 4 == 3) wc(10);
                }
                wc(10);
            }
            w("--- /proc/self/maps ---"); wc(10);
            fsync_out();
            int mf = open("/proc/self/maps", O_RDONLY);
            if (mf >= 0) {
                char tmp[4096]; ssize_t n;
                while ((n = read(mf, tmp, sizeof(tmp))) > 0) { ssize_t r = write(fd, tmp, (size_t)n); (void)r; }
                close(mf);
            }
            w("--- end ---"); wc(10);
            fsync_out();
            if (fd >= 0) { close(fd); fd = -1; }
        }
    }
    /* Let the process die the way it would have: restore its own handler and re-raise. */
    if (have_app[sig]) {
        sigaction(sig, &app_act[sig], NULL);
        raise(sig);
        /* A handler that returns (breakpad's usually does not) falls through to the default. */
        signal(sig, SIG_DFL);
        raise(sig);
    }
    _exit(139);
}

static int is_fatal(int sig) {
    return sig == SIGSEGV || sig == SIGBUS || sig == SIGABRT || sig == SIGILL || sig == SIGFPE || sig == SIGSYS;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    static int (*real)(int, const struct sigaction *, struct sigaction *);
    static int mine = -1;
    if (!real) real = (int (*)(int, const struct sigaction *, struct sigaction *))dlsym(RTLD_NEXT, "sigaction");
    if (mine < 0) {
        /* Only the client under study. Wrapping every process's handlers would change how the
         * compositor and the X server handle their own faults, which is not this shim's business. */
        char comm[64] = {0};
        int cf = open("/proc/self/comm", O_RDONLY);
        if (cf >= 0) { ssize_t n = read(cf, comm, sizeof(comm) - 1); (void)n; close(cf); }
        for (char *q = comm; *q; q++) if (*q == 10) { *q = 0; break; }
        mine = strcmp(comm, "steam") == 0 || getenv("CRASHCATCH_ALL") != NULL;
    }
    if (mine && signum > 0 && signum < NSIG && is_fatal(signum) && act && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN) {
        /* The process wants a handler for a fatal signal - keep it, and put ours in front of it. */
        app_act[signum] = *act;
        have_app[signum] = 1;
        struct sigaction mine;
        memset(&mine, 0, sizeof(mine));
        mine.sa_sigaction = handler;
        mine.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&mine.sa_mask);
        return real(signum, &mine, oldact);
    }
    return real(signum, act, oldact);
}

__attribute__((constructor)) static void install(void) {
    /* No handler of our own yet - sigaction() above installs it the moment the process asks for one,
     * which is what makes this survive breakpad's later call. Mark loaded so a log shows the shim is
     * in the process. */
    int f = open("/tmp/crashcatch.loaded", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (f >= 0) { ssize_t r = write(f, "loaded\n", 7); (void)r; close(f); }
}
