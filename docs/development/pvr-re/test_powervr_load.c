/* Behavioral reverse engineering harness */
#include <stdio.h>
#include <dlfcn.h>
int main() {
    printf("Attempting load of PowerVR driver...\n");
    /* Try to load the bionic .so from glibc */
    void* h = dlopen("/vendor/lib64/hw/vulkan.powervr.so", RTLD_NOW);
    if (h) printf("SUCCESS: Driver loaded (unexpected in glibc — needs libhybris)\n");
    else printf("EXPECTED FAILURE: Bionic .so in glibc (this is what libhybris fixes)\n");
    printf("Next: Apply LD_PRELOAD libhybris shim, retry\n");
    return 0;
}
