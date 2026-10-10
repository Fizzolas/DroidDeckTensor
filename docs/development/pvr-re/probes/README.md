# PowerVR probe sources (verified 2026-10-10)

Self-contained C probes used to establish the ground truth in
`docs/development/tensor-g5-powervr-state.md`. No Vulkan headers needed; types are local.

| File | What it does | How to build |
|---|---|---|
| `pvr_vkprobe.c` | Enumerates instance/device extensions and the core feature words through the **system loader** (`libvulkan.so`). | NDK: `aarch64-linux-android26-clang -O2 -o pvr_vkprobe pvr_vkprobe.c` |
| `pvr_hal_probe.c` | Drives the vendor **HAL** directly: `dlopen` the blob, resolve `HMI`, `open("vk0")`, `vkCreateInstance`, `vkEnumeratePhysicalDevices`, format/feature queries. | NDK (bionic, runs on device); or zig `cc -target aarch64-linux-gnu` for the glibc half of the experiment |
| `pvr_drmver.c` | Reads the DRM driver name/version of the GPU nodes (`DRM_IOCTL_VERSION`). Shows `pvr` 25.3 — the name Mesa's pvrsrvkm winsys expects. | NDK clang |
| `wsi_client.c` + `wsi_compositor.c` (+`run_wsi.sh`) | Drives the ICD's Wayland WSI: the client creates a wl_surface, a wayland surface, a swapchain, renders, and calls vkQueuePresentKHR; a separate-process minimal compositor records the attach/commit. | zig cc with the wayland stacks staged |
| `vk_dmabuf_rt_test.c` | Renders a triangle into an exportable image, exports the dma-buf fd, **imports it back**, and verifies the pixels via a GPU copy — the exact compositor round trip WSI needs. | zig cc `-I<VulkanSDK>/Include` |
| `vk_dmabuf_test.c` | Creates an exportable image, allocates exportable memory, and `vkGetMemoryFdKHR` — proves the driver exports a displayable **dma-buf** (the guest compositor's presentation mechanism) from glibc. | zig cc `-I<VulkanSDK>/Include` |
| `vk_tri_test.c` (+`tri.vert`/`tri.frag`/`tri_spv.h`) | Rasterises an offscreen red triangle (small, so a corner stays black) and reads the pixels back — proves **graphics** rasterisation through the ICD under glibc. | zig cc `-I<VulkanSDK>/Include` |
| `vk_compute_test.c` (+`pattern.comp`, `pattern_spv.h`) | Builds a compute pipeline, dispatches `data[i]=i*3+7`, reads back and verifies — proves the GPU **executes** through the shim ICD under glibc. | zig cc `-I<VulkanSDK>/Include`, link `-ldl` |
| `pvr_hybris_test.c` | Loads the bionic HAL through **libhybris** in a glibc process and enumerates the GPU — the load-half proof. | zig `cc -target aarch64-linux-gnu`, link `-L<dir> -l:libhybris.so` (NOT the linker DSO) |
| `pvr_dltest.c` | `dlopen`s one library and prints the result — used to find which bionic libs survive a glibc loader (none do; each traps). | zig `cc -target aarch64-linux-gnu -O2 -o pvr_dltest pvr_dltest.c -ldl` |

Run on-device with `adb push ... /data/local/tmp/ && adb shell chmod 755 ... && ./probe`.
For the glibc variants, an Ubuntu arm64 base rootfs' `usr/lib/aarch64-linux-gnu/` is staged beside the
binary and run as `./ld-linux-aarch64.so.1 --library-path ./lib:/system/lib64:/vendor/lib64 ./probe`.
