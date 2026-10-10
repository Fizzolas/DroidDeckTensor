# Tensor G5 (Pixel 10) PowerVR — verified state and the path to Steam

Status: **ground truth established on hardware, 2026-10-10.** This document supersedes the earlier
PowerVR material under `docs/development/pvr-re/` (`REVERSE_ENGINEERING_REPORT.md`, `AUDIT_FINAL.md`,
`behavioral_diff.md`, `device_probe.log`), several of whose claims are provably wrong — see §5. It also
supersedes the Mali plan in `google-tensor-support.md` for Tensor G5, because **G5 is not a Mali**.

Everything here was produced by running commands against an attached Pixel 10 Pro XL and disassembling
the vendor binaries; where a claim is inferred rather than measured it is labelled so.

---

## 1. The device, measured

| Fact | Value | How |
|---|---|---|
| Model / board | Pixel 10 Pro XL / `mustang` | `adb shell getprop ro.hardware` |
| SoC | Google **Tensor G5** | `getprop ro.soc.model` |
| Platform code name | **`laguna`** | `getprop ro.board.platform` |
| OS | **Android 17** | `getprop ro.build.version.release` |
| GPU | **PowerVR D-Series DXT-48-1536 MC1** | `vulkan.powervr.so` / vkjson |
| GPU ids | vendor `0x1010` (Imagination), device `0x71061212` | `vkGetPhysicalDeviceProperties` |
| System Vulkan | **1.4.317**, driver `6908880` (`0x696bd0`) | HAL probe, vkjson |

Kernel GPU stack:

* module **`pvrsrvkm`** (`/proc/modules`), a proprietary Google/Imagination DDK; module parameters
  include `RGXBVNC`, `IgnoreHWReportedBVNC`, `DriverMode`.
* DRM nodes: **`/dev/dri/renderD128`** → `34f00000.gpu0`, `uevent` `DRIVER=pvrsrvkm`,
  `OF_COMPATIBLE=img,gpu`; also `/dev/dri/card2`. `/dev/pvr_sync` (misc, `10,285`).
* **No `/dev/pvrsrvkm`. No `/sys/class/pvr_sync`** (the class is `/sys/class/misc/pvr_sync`).
* **No `CONFIG_DRM_POWERVR`** in `/proc/config.gz` — the mainline `drm/imagination` driver is not built.

Vulkan capability (the system loader's own probe, `/data/local/tmp/pvr_vkprobe`):

* 161 device extensions, including everything DroidDeck's compositor and a DXVK stack ask for:
  `VK_EXT_image_drm_format_modifier`, `VK_EXT_external_memory_dma_buf`, `VK_KHR_external_memory_fd`,
  `VK_KHR_external_semaphore_fd`, `VK_EXT_physical_device_drm`,
  `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_KHR_timeline_semaphore`,
  `VK_KHR_dynamic_rendering` (+`local_read`), `VK_EXT_descriptor_indexing`,
  `VK_EXT_descriptor_buffer`, `VK_KHR_robustness2`, `VK_KHR_present_wait2`.
* Texture compression: `BC1/BC2/BC3` (S3TC) are **samplable**; `BC7` is not; `ETC2`/`ASTC` are.

---

## 2. The vendor userspace stack, measured

`/vendor/lib64/hw/vulkan.powervr.so` — 3,167,784 bytes, md5 `737145ae0cea503d755d99dcbbd6e44d`,
**valid ELF, not stripped, disassembles cleanly** (`llvm-objdump`).

* It is an Android **hwvulkan HAL module**, not a standard Vulkan ICD. It exports the hal module
  symbol **`HMI`** (`hw_module_t`: id `vulkan`, name *"PowerVR D-Series Vulkan HAL"*, author
  *"Imagination Technologies"*). Its `vk_icdCreateInstance`/`vk_icdGetInstanceProcAddr` are **local
  symbols** (`t`), so a loader cannot `VK_ICD_FILENAMES` it. Its module `open()` accepts the id
  **`"vk0"`** (other ids are refused with `EINVAL`), and returns a device whose
  `CreateInstance` is at `+0x80` and `GetInstanceProcAddr` at `+0x88`.
* bionic: links `libc.so`/`libc++.so` (no `libc.so.6`); NEEDED `libcutils, libion, liblog,
  libnativewindow, libsync, libcustomer_gralloc_ddk_api, libsrv_um, libufwriter, libusc,
  aconfig_gpu_flags_c_lib, aconfig_gpu_img_flags_c_lib`.
* Call chain: `vulkan.powervr.so` → **`libsrv_um.so`** (the `PVRSRVBridge*` marshalling layer that
  issues the kernel ioctls) → `pvrsrvkm`. `libusc.so` is the shader compiler. `libufwriter.so` is
  52 MB of firmware/uniflex data.
* `libsrv_um.so` NEEDED is binder-free: `libdrm, libdmabufheap, libion, libsync, libcutils,
  libnativewindow, libutils, libutilscallstack, liblog` (+ c++/c/m/dl).
* `libcustomer_gralloc_ddk_api.so` NEEDED **does** pull binder HALs:
  `android.hardware.graphics.allocator-V2-ndk`, `android.hardware.graphics.mapper@4.0`,
  `libbinder_ndk`, `libvndksupport`, `libgralloctypes`.

**Direct-drive result (`probe/pvr_hal_probe.c`, built bionic, run on device):** dlopen-ing the blob,
resolving `HMI`, calling `open("vk0")`, then `vkCreateInstance` **works** and returns one physical
device `PowerVR D-Series DXT-48-1536 MC1` at Vulkan 1.4.317. The HAL is usable through its own
entrypoints without the Android graphics stack; only the *loader's* ICD path is unavailable.

---

## 3. What works today, and what cannot

**Works.** The system Vulkan driver enumerates and is feature-rich; the compositor/surface-sharing
primitives (dma-buf, DRM modifiers, external memory/semaphore FDs, AHB) are all advertised; the device
renders Android's own UI through this driver.

**Cannot work yet.** DroidDeck runs games in a **glibc** rootfs (proot → Proton → FEX → DXVK). There is
no glibc Vulkan driver for this GPU:

* the vendor driver is a bionic HAL and cannot be `dlopen`ed by a glibc process, and cannot be handed
  to a loader as an ICD;
* Mesa's PowerVR driver cannot bind this kernel — it wants the **mainline `drm/imagination`** uapi
  (`PVR_DRM_DRIVER_NAME "powervr"`) or the proprietary `pvrsrvkm` services protocol **v1.17**, builds
  **Rogue only** (`mesa/src/imagination/vulkan/meson.build`, `foreach arch:['rogue']`), and has no
  DXT-48-1536 BVNC;
* the guest is shipped only `libvulkan_freedreno.so` (a Turnip), which has no backend for anything on
  this device.

So a session today gets a system-Vulkan compositor and a guest with **no Vulkan device at all**.

---

## 4. The path, ranked

1. **libhybris-style bionic-in-glibc shim (recommended — the load half is DONE).** A glibc
   `libvulkan_powervr.so` that `android_dlopen_ext`s the vendor HAL (and `libsrv_um`/`libusc`), drives
   it through `HMI`/`open("vk0")` exactly as `pvr_hal_probe.c` does, and translates the Android bits the
   vendor stack touches: gralloc/AHB → dma-buf, `/dev/pvr_sync` fences, and a WSI for the guest's
   Wayland/X11. **As of 2026-10-10 the driver-loading half is proven** (see §4a): the bionic HAL loads
   and enumerates the GPU from a glibc process via libhybris. What remains is the ICD wrapper, the
   gralloc/dma-buf export, the fence translation and the WSI — real work, but no longer an unknown.
   Recipe: `pvr-re/hybshim/`.
2. **bionic producer + dma-buf handoff.** Render guest surfaces on a bionic helper (the app already
   loads drivers via AdrenoTools) and hand dma-bufs to the guest compositor. Solves presentation, not the
   game process's own D3D→Vulkan work, so it does not by itself make a game render.
3. **Mesa PVR + a `pvrsrvkm`-ioctl backend + Volcanic/BVNC support.** Clean-licence, but the kernel
   ABI must be bridged and Volcanic added upstream. Years, unproven.
4. **Mainline `drm/imagination` kernel on the Pixel.** Bootloader + kernel replacement; not near-term.

### The decisive experiment, run (2026-10-10)

`pvr_hal_probe.c` was compiled two ways — NDK/bionic (runs on-device) and, with the zig toolchain,
glibc aarch64 — and run against the same blob:

* **bionic: works.** `HMI` → `open("vk0")` → `vkCreateInstance` → one device
  `PowerVR D-Series DXT-48-1536 MC1` at 1.4.317, with BC1/2/3 samplable. The HAL is fully usable
  through its own entrypoints.
* **glibc: fails at the first step.** Loaded with the glibc loader
  (`ld-linux-aarch64.so.1 --library-path ./lib:/system/lib64:/vendor/lib64`), *every* bionic library
  — `liblog.so`, `libdrm.so`, `libcutils.so`, … — **traps (SIGTRAP)** during `dlopen`. `/system/lib64/libc.so`
  is the real bionic libc and imports loader-only symbols (`__loader_android_get_exported_namespace`)
  that only Android's `linker64` satisfies; the glibc loader cannot resolve them.

So a bionic driver **cannot simply be dlopen-ed by a glibc process**: path 1 requires a real
libhybris-style shim that loads bionic libs through an emulated Android linker (or maps their symbols
onto glibc), not just a `dlopen`. That is the concrete work item, and the concrete risk.

### 4a. And then it was built — the wall is down (2026-10-10)

The libhybris shim was built and run on the device. In a **glibc** process:

```
android_dlopen_ext(/vendor/lib64/hw/vulkan.powervr.so) ... OK
HMI: name=PowerVR D-Series Vulkan HAL author=Imagination Technologies
open(vk0) rc=0
vkCreateInstance -> 0
RESULT: SUCCESS - 1 device(s): "PowerVR D-Series DXT-48-1536 MC1" api=1.4.317 vendor=0x1010
device extensions visible through glibc: 148   (drm_format_modifier, external_memory_fd, ...)
BC3_UNORM optimalTilingFeatures=0x0000d401  SAMPLED (DXVK DXTn OK)
```

And on top of it, a **glibc Vulkan ICD** (`pvr-re/hybshim/icd/pvr_icd.c`, built as
`libvulkan_powervr.so`) that the real glibc Vulkan loader discovers via `VK_ICD_FILENAMES`:
`vkCreateInstance` -> 1 device `PowerVR D-Series DXT-48-1536 MC1` (1.4.317), 1 queue family
(GRAPHICS, 2 queues), and **`vkCreateDevice` -> 0**. That is the whole chain the guest needs to
enumerate and open the GPU, from glibc.

Three consecutive runs, exit 0. So the bionic vendor driver **does** run under glibc — the trap that
`pvr_dltest.c` showed is exactly what libhybris's emulated bionic linker (`q.so`) and libc-hook layer
(`libhybris.so`) resolve. The build uses only `zig` (no Android SDK, no sudo); the recipe and the
required upstream patches are in `pvr-re/hybshim/`. This reorders the path: option 1 is no longer
"recommended but unproven" — its load half is proven, and the remaining work is the ICD/WSI/gralloc
layer, not an unknown.

The other sub-question that decides path 1 — whether the blob's kernel bridge works when the *driver*
is driven from a foreign libc rather than loaded by one — was **not** settled here: `libsrv_um.so` is
itself bionic, so it too traps under glibc. It needs the same shim to be exercised.

**Cheaper first probe than the full shim:** drive the blob bionically but under strace, and confirm it
reaches `pvrsrvkm` through `/dev/dri/renderD128`; then confirm the same connection succeeds when the
process is a guest uid, which is what a libhybris shim would be.

---

## 5. Committed claims that are false (do not build on them)

| Where | Claim | Reality |
|---|---|---|
| `REVERSE_ENGINEERING_REPORT.md` | blob is 3,185,046 B, stripped, "objdump: file format not recognized" | real blob is 3,167,784 B, **not** stripped, disassembles; the "blocked" copy is the corrupt `extracted/vulkan.powervr.so` |
| design spec, `AUDIT_FINAL.md` | system driver lacks `VK_EXT_image_drm_format_modifier` → Phase 1 black screen | **present**; Phase-1 blocker does not exist |
| `behavioral_diff.md`, `device_probe.log` | version "1.634.2906", "Android 16", "requires /dev/pvrsrvkm" | 1.662.3024 (`6908880`), Android 17, no such node |
| `GpuInfo.kt` (pre-`fd289b8`) | `POWERVR_CLASS=/sys/class/pvr_sync`, `POWERVR_NODE=/dev/pvrsrvkm` | `/sys/class/misc/pvr_sync`, `/dev/dri/renderD128` |
| `google-tensor-support.md` | "G4/G5 = Mali-G715 (kbase)" | true for G4, **false for G5 (PowerVR)** |
| `mesa/src/imagination/vulkan/pvr_arch_volcanic.c` | "derived register spec" | **invented constants** (Vulkan "1.2" vs actual 1.4.317); deleted |

---

## 6. App-side state

Committed in `fd289b8` (2026-10-10): the tree did not compile at `bb3f008` (`DriverPairs.recommendedKey`
had no `POWERVR_GXX` branch; `GpuVendorTest` used the old `vendorOf` signature). It now builds
(`assembleDebug` ✓; gpu + runtime unit tests green). PowerVR detection is honest (correct probe paths,
"PowerVR D-Series" family, `laguna` → Tensor G5, its own support wording), and the runtime's ICD
vendor / library / GPU node / driver name follow the device's stack instead of a mali/else split.

Still absent, by design until a driver exists: any bundled PowerVR driver, any PowerVR guest ICD, and
the real compositor present path for this device. `README.md` should keep saying PowerVR is not yet
supported until §4's path yields a working driver.

---

## 7. The built APK, on the device (2026-10-10)

Verified on the attached Pixel 10 Pro XL (Android 17), not just in a harness:

* The app assembles and installs, and reports the device as
  **"PowerVR D-Series: experimental: PowerVR support is new and untested"** (amber, not refused) —
  the detection, family name and wording from the app work land correctly on hardware.
* Opening the Components page installs the bundled guest driver the apk carries
  (`assets/linux_driver/powervr-g5x.tzst`) into `files/linux_vulkan_drivers/powervr-g5x/`: the ICD,
  `libhybris.so`, `q.so`, `libwayland-client.so.0`, `libffi.so.8`, plus a generated `icd.json`
  (absolute `library_path`, api 1.4.0) and `meta.json` (name "PowerVR (Tensor G5)", 1.4.317).
  That manifest is exactly what a session hands the guest as `BL_VK_DRIVER`, which the session
  script exports as `VK_DRIVER_FILES`/`VK_ICD_FILENAMES` — so a PowerVR session now points the
  guest's Vulkan loader at this ICD.

**Found while doing it, not fixed:** Android 17 shows the app a *16 KB page-alignment* warning —
several bundled native libraries (`libwayland-server.so`, `libdeviceinfo.so`, `libpulse*.so`,
`libltdl.so`, `libsndfile.so`, `libc++_shared.so`, …) are not 16 KB-aligned. The app targets SDK 28
(deliberately, so it may `exec` a rootfs from its data dir), so this is a warning rather than a
blocker today, but a future Play requirement and a real fix: link the NDK targets with
`-Wl,-z,max-page-size=16384` and rebuild the prebuilt libraries. It is unrelated to PowerVR and
affects the app as a whole.
