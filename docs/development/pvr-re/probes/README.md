# PowerVR probe sources (verified 2026-10-10)

Self-contained C probes used to establish the ground truth in
`docs/development/tensor-g5-powervr-state.md`. No Vulkan headers needed; types are local.

| File | What it does | How to build |
|---|---|---|
| `pvr_vkprobe.c` | Enumerates instance/device extensions and the core feature words through the **system loader** (`libvulkan.so`). | NDK: `aarch64-linux-android26-clang -O2 -o pvr_vkprobe pvr_vkprobe.c` |
| `pvr_hal_probe.c` | Drives the vendor **HAL** directly: `dlopen` the blob, resolve `HMI`, `open("vk0")`, `vkCreateInstance`, `vkEnumeratePhysicalDevices`, format/feature queries. | NDK (bionic, runs on device); or zig `cc -target aarch64-linux-gnu` for the glibc half of the experiment |
| `pvr_drmver.c` | Reads the DRM driver name/version of the GPU nodes (`DRM_IOCTL_VERSION`). Shows `pvr` 25.3 — the name Mesa's pvrsrvkm winsys expects. | NDK clang |
| `vk_compute_test.c` (+`pattern.comp`, `pattern_spv.h`) | Builds a compute pipeline, dispatches `data[i]=i*3+7`, reads back and verifies — proves the GPU **executes** through the shim ICD under glibc. | zig cc `-I<VulkanSDK>/Include`, link `-ldl` |
| `pvr_hybris_test.c` | Loads the bionic HAL through **libhybris** in a glibc process and enumerates the GPU — the load-half proof. | zig `cc -target aarch64-linux-gnu`, link `-L<dir> -l:libhybris.so` (NOT the linker DSO) |
| `pvr_dltest.c` | `dlopen`s one library and prints the result — used to find which bionic libs survive a glibc loader (none do; each traps). | zig `cc -target aarch64-linux-gnu -O2 -o pvr_dltest pvr_dltest.c -ldl` |

Run on-device with `adb push ... /data/local/tmp/ && adb shell chmod 755 ... && ./probe`.
For the glibc variants, an Ubuntu arm64 base rootfs' `usr/lib/aarch64-linux-gnu/` is staged beside the
binary and run as `./ld-linux-aarch64.so.1 --library-path ./lib:/system/lib64:/vendor/lib64 ./probe`.
