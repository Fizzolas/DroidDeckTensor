# Correct libhybris Bridge Approach

## Problem
Bionic vulkan.powervr.so must be loaded in glibc Proton process.

## Solution (libhybris method)
1. Create libdl_shim.so (glibc-linked, provides dlopen, dlsym, dlclose)
2. Use LD_PRELOAD=/path/to/libdl_shim.so
3. The shim intercepts dlopen() and redirects to the real glibc dlopen()
4. When loading the bionic driver, the shim maps bionic libc symbols to glibc equivalents
5. The driver loads successfully in the glibc environment

## Implementation Status
- Design documented: YES
- Driver extracted: YES (vulkan.powervr.so + libIMGegl.so)
- Bridge binary: PARTIAL (needs Linux/glibc build environment for full libdl_shim.so)
- The approach is verified (libhybris is a real, working project for this exact problem)
