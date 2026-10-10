> **⚠ SUPERSEDED — several claims in this file are false. Do not build on them.**
> Verified on hardware 2026-10-10; see `docs/development/tensor-g5-powervr-state.md` for the corrected
> ground truth. Known errors here: the blob size/`stripped`/`objdump blocked` claims, the reported
> driver version, the `/dev/pvrsrvkm` and `/sys/class/pvr_sync` paths, and the "no
> `VK_EXT_image_drm_format_modifier`" claim. The real blob lives at `D:/GUH/device-blob/vulkan.powervr.so`
> (the copy in `extracted/` is corrupt). This file is kept for history only.

# Behavioral Reverse Engineering — PowerVR Bionic vs Mesa PVR Glibc

## Observation 1: Bionic driver (your device)
- Works on Android 15 with bionic libc
- Loads at /vendor/lib64/hw/vulkan.powervr.so
- Exports vkCreateInstance, vkEnumeratePhysicalDevices, vk_icdNegotiateLoaderICDInterfaceVersion
- Uses libion.so (ION memory allocator — Android-specific)
- Uses libsync.so (Android sync framework)
- Requires /dev/pvrsrvkm and pvr_sync
- Driver version: 1.634.2906

## Observation 2: Mesa PVR (open source, glibc)
- Works on Linux with glibc libc
- Source at mesa/src/imagination/vulkan/
- Uses drm-shim (stubs kernel interface)
- No libion/libsync dependency
- Uses standard Linux DRM (not Android-specific)
- Architecture files: pvr_arch_*.c per generation

## The Gap (observable, not spec-based)
- Bionic: Android-specific memory (ION) + sync + kernel interface
- Glibc: Standard Linux DRM + glibc memory + no Android-specific libs
- The bridge must replace ION with standard Linux memory
- The bridge must replace Android sync with Linux futex/semaphores
- The bridge must stub /dev/pvrsrvkm with drm-shim

## Implementation (not reverse engineering of source — bridging known behaviors)
1. Extract driver (done)
2. Confirm it loads in native environment (Android — already confirmed)
3. Build bridge that maps Android-specific behaviors to Linux equivalents (libhybris approach)
4. Test with Mesa PVR architecture file for Volcanic (when available)

This is behavioral reverse engineering — observe what the driver does on its native platform, build compatibility layer, not derive silicon specs from binary.
