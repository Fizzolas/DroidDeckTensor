# PowerVR probe sources (verified 2026-10-10)

Self-contained C probes used to establish the ground truth in
`docs/development/tensor-g5-powervr-state.md`. No Vulkan headers needed; types are local.

| File | What it does | How to build |
|---|---|---|
| `pvr_vkprobe.c` | Enumerates instance/device extensions and the core feature words through the **system loader** (`libvulkan.so`). | NDK: `aarch64-linux-android26-clang -O2 -o pvr_vkprobe pvr_vkprobe.c` |
| `pvr_hal_probe.c` | Drives the vendor **HAL** directly: `dlopen` the blob, resolve `HMI`, `open("vk0")`, `vkCreateInstance`, `vkEnumeratePhysicalDevices`, format/feature queries. | NDK (bionic, runs on device); or zig `cc -target aarch64-linux-gnu` for the glibc half of the experiment |
| `pvr_dltest.c` | `dlopen`s one library and prints the result — used to find which bionic libs survive a glibc loader (none do; each traps). | zig `cc -target aarch64-linux-gnu -O2 -o pvr_dltest pvr_dltest.c -ldl` |

Run on-device with `adb push ... /data/local/tmp/ && adb shell chmod 755 ... && ./probe`.
For the glibc variants, an Ubuntu arm64 base rootfs' `usr/lib/aarch64-linux-gnu/` is staged beside the
binary and run as `./ld-linux-aarch64.so.1 --library-path ./lib:/system/lib64:/vendor/lib64 ./probe`.
