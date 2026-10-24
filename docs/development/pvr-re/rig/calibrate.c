/* calibrate - the known answer each guest instrument must reproduce before it is believed.
 *
 * mmwatch must show the mmap below with this caller; symwatch must show the dlopen; crashcatch
 * must report the address this prints; the marker proves the log route and the clock. A digest
 * producer needs none of this - its pattern check runs in the compositor and on the host.
 *
 * usage: calibrate mmap | dlopen | crash | mark <label>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CRASH_ADDR ((volatile unsigned char *)0xdeadbe0UL)

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "mmap")) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { perror("mmap"); return 1; }
        printf("CAL-MMAP %p %d\n", p, 4096);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(mode, "dlopen")) {
        void *h = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
        printf("CAL-DLOPEN libc.so.6 %p\n", h);
        fflush(stdout);
        return h ? 0 : 1;
    }
    if (!strcmp(mode, "mark")) {
        const char *label = argc > 2 ? argv[2] : "unnamed";
        printf("MARK-%s\n", label);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(mode, "crash")) {
        /* A fixed, computed-looking address in the same shape as the real faults (§63's si_addr
         * ended fc028 in every crash). Printing it first means a report can be matched to this
         * run rather than to a previous one. */
        printf("CAL-CRASH %p\n", (void *)CRASH_ADDR);
        fflush(stdout);
        signal(SIGSEGV, SIG_DFL);
        *CRASH_ADDR = 1;
        return 0;
    }
    fprintf(stderr, "usage: %s mmap|dlopen|crash|mark <label>\n", argv[0]);
    return 2;
}
