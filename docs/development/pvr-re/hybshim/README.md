# libhybris PowerVR shim — proven recipe

**Result (2026-10-10, on a Pixel 10 Pro XL):** the bionic `vulkan.powervr.so` HAL loads and
enumerates the GPU **from a glibc process**, via libhybris, on this device. `probes/pvr_hybris_test.c`
prints one physical device `PowerVR D-Series DXT-48-1536 MC1`, Vulkan 1.4.317, 148 extensions, and
BC3 (SXTn) sampled. Three consecutive runs, exit 0.

This is the wall that `docs/development/tensor-g5-powervr-state.md` §3 described as the blocker: a
bionic Android **Vulkan HAL** cannot be `dlopen`ed by a glibc loader (every bionic lib traps — see
`probes/pvr_dltest.c`). libhybris's emulated bionic linker loads it anyway, and its libc hooks satisfy
the loader-only symbols (`__loader_android_get_exported_namespace`) that made the trap.

## What is here

* `build_hybris.sh` — builds the two DSOs from a libhybris checkout. Reproducible, no Android SDK:
  uses `zig` (`zig cc`/`zig c++`) at `$HOME/zig-linux-x86_64-0.13.0`. Edit `CO=` to the checkout path.
* `config/` — the three tiny headers libhybris normally gets from autotools + the `android-headers`
  package: `config.h`, `android-config.h`, `cutils/native_handle.h`. Drop them into the checkout.
* `../probes/pvr_hybris_test.c` — the proof harness.

## The two DSOs (libhybris keeps them separate for a reason)

| DSO | Sources | Role |
|---|---|---|
| `q.so` (or `mm.so`) | `hybris/common/q/*` + vendored `bionic/libc` | the emulated bionic **linker**; exports `android_dlopen_ext`, `android_linker_init`, the `__loader_*` symbols |
| `libhybris.so` | `hybris/common/{hooks,wrappers,native_handle,…}.c` + `legacy_properties/` | the **libc hook** layer; exports glibc-compatible `dlopen`/`stdout`/`pthread_*`/… the bionic libs import |

At inference `hooks.c` `dlopen`s `"$HYBRIS_LINKER_DIR/q.so"`; keep both DSOs in the loader path.

## Required libhybris patches (clang 18 / newer glibc)

The upstream tree assumed an older clang and `android-headers`. Three edits make it build:

1. `common/config.h`, `common/android-config.h`, `include/cutils/native_handle.h` — add (copies in
   `config/`). The last is the `native_handle_t` struct.
2. `common/q/linker_debug.h`:
   * `#define LINKER_DEBUG_TO_LOG 0` (its bionic `async_safe/log.h` path assumes bionic).
   * `_PRINTVF` passes `std::vector<std::string>` to `fprintf` via `TRACE(...)`; clang 18 errors.
     Replace the body with a no-op (or a location-only `fprintf(stderr, "%s:%d\n", __FILE__, __LINE__)`
     for debugging — that is how the mm-vs-q confusion below was found).
3. Build with `-D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FORTIFY_SOURCE=1`, `-std=c++17`, and the zig
   `asm/` include dirs (`aarch64-linux-any` + `any-linux-any`) for `tlsdesc_resolver.S`.

> **Do NOT define `HYBRIS_SHM_MASK_TOP` yourself.** `hooks_shm.h` defines it correctly as
> `UINTPTR_MAX - 15`; a hand-written `#define HYBRIS_SHM_MASK_TOP 0` (which an early version of this
> recipe had) makes `hybris_check_android_shared_cond` classify *every* heap pointer as "shared with
> Android", so `_hybris_hook_pthread_cond_wait` returns immediately instead of blocking — device
> creation then spins forever. This one line cost hours; it is the whole reason `vkCreateDevice`
> hangs. The `config/android-config.h` here no longer defines it.

**Use the Q linker, not MM.** MM is Android 6 and rejects modern `R_AARCH64_TLSDESC` relocations
("unknown reloc type 1031"); Q handles them. Build q.so with `WANT_LINKER_Q`, and build `hooks.c`
with the same `-DWANT_LINKER_Q` so the two agree on `LINKER_NAME_DEFAULT`.

## Running it (as the eventual guest ICD will)

glibc runtime + both DSOs staged on device; run through the glibc loader:

```sh
./lib/ld-linux-aarch64.so.1 --library-path ./lib:./linkerdir ./probe/pvr_hybris_test
# env: HYBRIS_LINKER_DIR=<dir holding q.so>
#      HYBRIS_LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw
```

## The ICD: `libvulkan_powervr.so` (done — enumeration + queues)

`icd/pvr_icd.c` is a glibc Vulkan **ICD** built on the shim. It links `libhybris.so` (so the bionic
hook layer is present), `android_dlopen_ext`s the HAL, drives `HMI`/`open("vk0")`, and forwards
`vk_icdGetInstanceProcAddr` / `vk_icdGetPhysicalDeviceProcAddr` / `vk_icdNegotiateLoaderICDInterfaceVersion`
to the HAL's `GetInstanceProcAddr`. Build:

```sh
zig cc -target aarch64-linux-gnu.2.38 -fPIC -shared -O1 -o libvulkan_powervr.so icd/pvr_icd.c \
  -L<links> -l:libhybris.so                 # <links> holds libhybris.so
```

Manifest for the guest (`VK_ICD_FILENAMES`):

```json
{"file_format_version":"1.0.0","ICD":{"library_path":"/usr/lib/libvulkan_powervr.so","api_version":"1.4.0"}}
```

**Proven (2026-10-10):** the real glibc Vulkan loader (`libvulkan.so.1`) discovers this ICD and
drives the GPU — `vkCreateInstance` → 1 device `PowerVR D-Series DXT-48-1536 MC1`, 1 queue family
(GRAPHICS, 2 queues), `vkCreateDevice` → 0. See `probes/vk_loader_test.c`. In DroidDeck terms
the guest would ship `libvulkan_powervr.so` (built by this repo) + the two shim DSOs + the manifest,
and `LinuxVulkanDriverManager` / `BwrapSpawner` already look for exactly that name.

**And the GPU executes:** `probes/vk_compute_test.c` builds a compute pipeline (the driver compiles the
SPIR-V through `libusc`), dispatches `data[i] = i*3+7`, waits for the queue, reads back and verifies —
**256/256 correct**. So the bionic driver does real GPU work under glibc, not just enumeration.

## Known remaining work

* The process must link the hook DSO only; linking the linker DSO **too**, or leaving mm.so in the
  path, mixes two linkers and segfaults in `prelink_image`. The linker DSO is dlopen'd at runtime.
* glibc's stack canary and bionic's `__stack_chk_guard` differ; `libhybris.so` must export the guard
  (it does) or teardown trips `__stack_chk_fail`. Harnesses should `_exit()` after their proof.
* The present path works (Wayland WSI, dma-buf swapchain, present). Remaining for a game to *render
  on screen*: bundle the ICD + shim into the guest runtime, and (for D3D titles) the same for DXVK.
  X11 WSI (gamescope Xwayland) is still only Wayland here; gralloc/AHB and /dev/pvr_sync are not
  wired yet, but neither is needed for the Wayland path the guest's gamescope uses.
