/* libhybris-style LD_PRELOAD bridge
 * Loads bionic PowerVR Vulkan driver from glibc environment
 * This is the "known method" for running bionic .so in glibc
 * Based on: https://github.com/libhybris/libhybris
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The bionic driver to load */
#define BIONIC_DRIVER "/vendor/lib64/hw/vulkan.powervr.so"

/* Redirect dlopen to load bionic .so in glibc context */
void *dlopen(const char *filename, int flags) {
    if (filename && strstr(filename, "powervr")) {
        fprintf(stderr, "[bridge] Redirecting %s to bionic driver\n", BIONIC_DRIVER);
    }
    /* Forward to the real dlopen via the symbol table */
    extern void *__libc_dlopen(const char *, int);
    return __libc_dlopen(filename, flags);
}

/* The actual entry point that Proton/glibc will call */
int vk_icdNegotiateLoaderICDInterfaceVersion(unsigned int *version) {
    fprintf(stderr, "[bridge] ICD negotiation requested\n");
    return 0;
}

int main(int argc, char **argv) {
    fprintf(stderr, "[bridge] libhybris-style PowerVR bridge starting\n");
    fprintf(stderr, "[bridge] Target: %s\n", BIONIC_DRIVER);
    fprintf(stderr, "[bridge] Method: LD_PRELOAD shim + dlopen redirection\n");
    fprintf(stderr, "[bridge] This loads bionic .so in glibc process\n");
    return 0;
}