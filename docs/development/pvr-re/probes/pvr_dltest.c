/* pvr_dltest.c — load one library and report, to find which bionic libs survive a glibc loader. */
#include <stdio.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: dltest <lib>\n"); return 2; }
    printf("dlopen(%s) ...\n", argv[1]);
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("  FAIL: %s\n", dlerror()); return 1; }
    printf("  OK: %p\n", h);
    void *s = dlsym(h, "HMI");
    if (s) printf("  HMI=%p\n", s);
    return 0;
}
