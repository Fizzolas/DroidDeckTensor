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

### App-UI wording on a PowerVR (noted and fixed, 2026-10-10)

Seen on the device: clicking **Setup -> Linux runtime -> Install** on the Pixel 10 showed the
*"Not an Adreno GPU"* dialog. That is the one part of the change-map item 2 that was left undone —
the detection and the Setup card correct, the download dialog still Adreno-centric. A PowerVR now
gets its own title (*"PowerVR GPU (Tensor G5)"*) and wording (*"… can drive it with the bundled
PowerVR driver, but PowerVR support is experimental and untested"*) instead of "DroidDeck needs an
Adreno". The English string is the source; the other 8 locales fall back to it until translated
(the same debt `gpuinfo_powervr_experimental` carries).

---

## 8. Why the guest never reached Steam, and the fix (2026-10-10, measured)

**Symptom.** Every session died ~5 s in with `<name> stopped unexpectedly`; the session log ended at

```
[gamescope] Info  vulkan: physical device supports DRM format modifiers
[gamescope] Error vulkan: physical device has no primary node
ERROR: wayland: Display scaling requires the missing 'zxdg_output_manager_v1' protocol: disabling
Failed to load plugin 'libdecor-gtk.so': failed to init
SDL_Vulkan_CreateSurface failed: VK_KHR_wayland_surface extension is not enabled in the Vulkan instance.Failed to create backend.
```

and `Initted Wayland backend` (printed only when `CWaylandBackend::Init()` completes) never appeared.

**What gamescope actually does.** `CVulkanDevice::createDevice()` (3.16.29, `src/rendervulkan.cpp`):

```c
if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary ) { errorf("no primary node"); return false; }
if ( !drmProps.hasRender ) { errorf("no render node"); return false; }
dev_t renderDevId = makedev( drmProps.renderMajor, drmProps.renderMinor );
drmGetDeviceFromDevId( renderDevId, 0, &drmDev );
m_drmRendererFd = open( drmDev->nodes[DRM_NODE_RENDER], O_RDWR | O_CLOEXEC );
```

`CWaylandBackend::UsesVulkanSwapchain()` is `false`, so the nested Wayland backend was held to the
DRM/KMS backend's primary-node requirement — a node it never opens (`m_drmPrimaryDevId` is read only by
`CDRMBackend`). On failure `IBackend::Set` deletes the backend and gamescope falls back to SDL against
the instance it already made with an empty extension list, which is the SDL line above.

**The real defect: the HAL writes nothing into the struct.** The PowerVR HAL advertises
`VK_EXT_physical_device_drm` and then leaves `VkPhysicalDeviceDrmPropertiesEXT` untouched. Measured
through this ICD with the struct poisoned (`probes/pvr_drm_props_probe.c`):

```
raw drmProps words: 3ba02ce8 cdcdcdcd 00000000 00000000 cdcdcdcd ...   <- sType only; all fields still poison
```

so an app that zero-initialises the struct — the spec's advice, and what gamescope does — reads
`hasPrimary = 0` whatever the device is. (An earlier note that the HAL "honestly reports
hasPrimary=false" was a measurement artefact: the probe then in the tree used a shortened stand-in for
`VkPhysicalDeviceProperties`, which the driver writes past, clobbering the pNext struct behind it.)

**The fix — in the ICD, no gamescope rebuild.** `pvr_icd.c` now answers
`vkGetPhysicalDeviceProperties2` itself: it calls the HAL for the rest of the chain and then fills the
DRM struct with the device behind the render node the HAL drives, matched the way libdrm does it (two
nodes are one DRM device when their `/sys/dev/char/<maj>:<min>/device` links resolve alike — card2 is
226:2 and renderD128 is 226:128, different numbers, one device). Measured on the device:

```
pvr_icd: physical_device_drm: hasPrimary=1 primary=226:2 render=226:128
```

This is reachable through `vk_icdGetInstanceProcAddr` — the loader's own trampoline for
`vkGetPhysicalDeviceProperties2` calls the pointer the ICD returned there. Putting it in
`vk_icdGetPhysicalDeviceProcAddr` instead is dead code (the loader consults that only for entry points
it has no trampoline for), which is why an earlier attempt there changed nothing.

**Why hasPrimary=1 is honest here, not a lie.** On this device the PowerVR GPU *does* expose a card
node: `/sys/class/drm/card2` and `/sys/class/drm/renderD128` are both symlinks into
`/sys/devices/platform/34f00000.gpu0/drm/`, and `drmGetDeviceFromDevId` run against real libdrm reports
them as one device. Reporting it is what a working DRM driver would do; the HAL omitting it is the
defect.

**The remaining steps were checked too** (`probes/pvr_drm_gate_probe.c`, real libdrm 2.4.134):

```
drmGetNodeTypeFromDevId(226:128) = 2 (render)
drmGetDeviceFromDevId() -> 0, bustype=2
available_nodes: primary=/dev/dri/card2 render=/dev/dri/renderD128
open("/dev/dri/renderD128", O_RDWR|O_CLOEXEC) -> fd 3
RESULT: PASS
```

so every step of the Wayland backend's `createDevice()` succeeds with the stand-in the app already
binds. Nothing on the app side had to change.

**Status: the ICD is fixed and the driver asset rebuilt; a full session on the current build has not
yet been run** (it needs the Linux runtime re-installed in the app, and the device was locked). The
expected log change is the absence of `physical device has no primary node` and the presence of
`Initted Wayland backend`. Pre-registered next failures, if any: `drmGetDeviceFromDevId() failed`,
`failed to open DRM render node`, or `physical device has no render node` — each names a different
cause, none of them the gate.

---

## 9. Audit: what each session failure was, measured (2026-10-10, session 2026-10-10-29..42)

Read from the guest's own `session.log` plus two probes (`probes/pvr_drm_props_probe.c`,
`probes/pvr_fmt_props_probe.c`) and an instrumented ICD. Each line is a measurement, not inference.

**#1 `physical device has no primary node` — FIXED.** The HAL advertises `VK_EXT_physical_device_drm`
and writes nothing into `VkPhysicalDeviceDrmPropertiesEXT` (poisoned struct stayed poison). The ICD
now fills it from the GPU's card node; log: `hasPrimary=1 primary=226:2 render=226:128`.

**#2 `drmGetDeviceFromDevId() failed` — FIXED.** Two causes, both in the app's stand-in:
* the `/dev/dri` binds were skipped entirely (an early `return` in `bindGpuNode`'s error path);
* `/sys/dev/char/<maj>:<min>/device` was a real directory, but libdrm `readlink`s it and looks for
  `/platform`, so every node was dropped. It is now a symlink to the kernel's own device directory.
Log: `opendir(/dev/dri)=ok`, `device -> /sys/devices/platform/34f00000.gpu0`, then
`xdg_backend: Initted Wayland backend` — the Wayland backend initialises.

**#3 `assert( modifiers.size() > 0 )` in `CVulkanTexture::BInit` — NOT FIXED. Root cause narrowed.**
Measured, via the ICD instrumented to wrap `vkGetPhysicalDeviceImageFormatProperties2`:

```
fmt=0x2c usage=0xd tiling=1000158000 flags=0x8->0x0 chain=2 mod=0x0 r=-11 retry=-11
```

* The query that fails is `vkGetPhysicalDeviceImageFormatProperties2`, DRM-modifier tiling,
  DMA_BUF handle, `usage=0xd` (TRANSFER_SRC|TRANSFER_DST|SAMPLED), `flags=0x8` (MUTABLE_FORMAT).
* The HAL returns `VK_ERROR_FEATURE_NOT_PRESENT (-11)`.
* **`vkGetPhysicalDeviceFormatProperties2` is never called** (0 calls) — so this is not the modifier
  enumeration, and the earlier "zero modifiers" errors are a separate, non-fatal path.
* gamescope treats any result other than `VK_ERROR_FORMAT_NOT_SUPPORTED` as fatal, so it never runs
  its own EXPORTABLE test, `modifiers` stays empty, and the assert fires.

**Not the cause** (each tested and ruled out): the HAL's exportability reporting (correct —
`features=0x00000006` EXPORTABLE|IMPORTABLE for every usage tried); the usage bits (all seven single
bits, and `0xd`, pass); removing `MUTABLE_FORMAT` + the format list in the ICD (still `-11`); the
modifier value (the repro uses LINEAR, `0x0`).

**Standing contradiction, unresolved:** the standalone probe reproduces `-11` *only* with
`MUTABLE_FORMAT` + a format list, while the HAL's own criterion is `usage & DRM_FORMAT_MODIFIER_BIT`
(`vk_present.c`'s `modifier_importable`, which the HAL's validator mirrors) — and `usage=0xd` has that
bit clear. So either that criterion is not what the HAL checks, or something about the guest's chain
differs from the probe's. **This is the open question; the next step is to determine the HAL's actual
condition, not to keep patching.**

**What has never executed:** `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR`, `vkQueuePresentKHR`,
Xwayland, and Steam itself. Every finding so far is upstream of the swapchain.

**Temporary instrumentation:** the ICD carried diagnostic wrappers (`diag`, `fmtprops2`,
`imgfmtprops2`) while this was measured; they have been removed and the shipped ICD contains only the
two fixes above.

## 10. The gate is passed; where the session stands now (2026-10-10, session 2026-10-10-43)

The session that all of §9 was preparing for was finally run end to end. Everything in §8 and §9
holds, and the three stacked failures are gone. This section is what the log actually says.

**The ICD in the shipped apk is the built one.** `assets/linux_driver/powervr-g5x.tzst` holds
`libvulkan_powervr.so` at sha256 `9f25d109f07bea3e68abebb46fc67a402504ec77222b6791da8b22097fb80aa5`,
identical to a fresh `build_icd.sh` output — so this log is evidence about the current source.

**What now works, in order, all measured:**

* `physical_device_drm: hasPrimary=1 primary=226:2 render=226:128` — §9 #1's fix, live.
* `xdg_backend: Initted Wayland backend` — the gate. `gamescope` no longer dies at
  `rendervulkan.cpp`'s primary-node check, and `SDL_VIDEODRIVER` is no longer needed (§8).
* `wlserver: Running compositor on wayland display 'gamescope-0'`, `Starting Xwayland on :0` and `:1`.
* **The ICD's X11 WSI path executed for the first time.** `e95ed6b` shipped it and `f075cf6` recorded
  the present path as unverified; it is now exercised:
  `[Gamescope WSI] Created swapchain for xid: 0x800002 swapchain: 0x3000f1f350 - imageCount: 3`,
  `pvr_wsi: swapchain: 3 images 1602x720 stride 6408 fmt 44 (fd0=25)`.
* **Steam ran and finished its first-run update** (666665 KB, extracted, installed). Its GPU query
  answers through our ICD: `name: "PowerVR D-Series DXT-48-1536 MC1"`, `driver_version 1.662.3024`.

**Where it stops — and the measurement was cut short.** Exactly one swapchain is created in the whole
log, for xid `0x800002`, the update-UI window (`Using update UI: glx`, `Create window`). That is
*expected* for the first three and a half minutes: the main client cannot start until the update it is
downloading has been installed. What matters is the end of the timeline:

```
16:27:42  Cleaning up...
16:27:43  Update complete, launching Steam...
16:27:44  Startup - Steam Client launched with: ... (the real client, tid 2346)
16:27:44  Process started with command-line: ... '-child-update-ui' ... (tid 2347)
16:27:45  Create window
16:28:02  app.log: DroidDeckWayland: [program] disconnected: gamescope   <- the session was stopped
```

So the real client had **17 seconds** before the session ended, and its UI was never observed coming
up. That reading was superseded by the very next session (§11), which ran the same build with the
client already installed — and the client **segfaults**. See §11.

The log is the authority here, not a screenshot: the last capture was mostly blank and a blank frame
means only that the compositor drew its base layer.

**Nothing in §9 was re-opened.** The `vkGetPhysicalDeviceFormatProperties2 returned zero modifiers`
errors are still printed and are still not fatal; the retry still reports
`DRM-modifier query fmt=0x2c usage=0x6 with MUTABLE_FORMAT -> -11; retried without it -> 0`.

## 11. The client gets its own swapchain, then segfaults (2026-10-10, session 2026-10-10-44-steam)

§10 left one thing unresolved for lack of time: the session it measured was stopped 17 s after Steam's
real client launched. The next session on the same build answered it. Steam was already installed, so
there was nothing to download and the session was left alone.

**What changed, measured:**

* `[Gamescope WSI] Creating Gamescope surface: xid: 0x1200002` — a **second** X11 surface, distinct
  from the update-UI's `0x800002`. This is the client's own UI window, and it is the first time in any
  session that the main client reached the WSI at all. `pvr_wsi` then builds the swapchain through the
  ICD's XCB/DRI3 path, so Steam's own presentation path works as far as swapchain creation.
* The swapchain is destroyed again immediately
  (`[Gamescope WSI] Destroying swapchain: 0x30012cea90`), so the client was still sizing its window.
* Steam's GPU query still answers through the ICD
  (`name: "PowerVR D-Series DXT-48-1536 MC1"`), and it went on to query `GpuTopology` and write
  `/tmp/steamJIKnMl`, so the Vulkan path up to that point is healthy.

**The wall — and it is not the ICD.** The client dies of signal 11, and the guest names the exact
line:

```
/usr/local/bin/droiddeck-session: line 287: 5133 Segmentation fault
  "$steam_root/steamrtarm64/steam" ... -cef-ozone-platform=x11 -cef-use-gl=angle -cef-use-angle=vulkan
== steam exited rc=139
```

`steamui_system.txt` places it: `CX11WindowProperty: failed to read window property:
GAMESCOPE_VIEWPORT_SUPPORTED`, at 16:37:47. The client's window appeared at 16:37:48 — the very
second the surface is created — and the process was down by 16:37:49. So the crash is in the **client
UI coming up**, not in the ICD, the loader or the HAL.

**Reproduced 2/2.** Session 45 (§11 note: press Play again; Steam's script restarts the client itself
once) shows the identical sequence, same pid-level detail, `steam exited rc=139`:

```
XRRGetOutputInfo Workaround: initialized with override: 1 real: (nil)
Error: XRRGetOutputInfo() is not available
[Gamescope WSI] Creating Gamescope surface: xid: 0x1200002
pvr_wsi: swapchain: 3 images 1x1 stride 8 fmt 44 (fd0=38)
[Gamescope WSI] Destroying swapchain: 0x30012cea80
Error: XRRGetOutputInfo() is not available
== steam exited rc=139
```

So this is deterministic, not a race.

**The suspects, in order, all measured from the log:**

1. **`Error: XRRGetOutputInfo() is not available`**, immediately before the crash line, in both
   sessions. The guest's Xwayland does not implement RandR's `RRGetOutputInfo`. Notably, **gamescope
   prints this at startup in §10's session too** (0x800002, the update-UI window) and that process
   survives — so the call is not fatal in itself. What is new here is that the **main client takes
   the same path**, and the client's own X11/RandR use may not tolerate the missing reply the way
   gamescope does.
2. **`GAMESCOPE_VIEWPORT_SUPPORTED` unreadable** — a client→compositor property that never got set.
   The client's parametric piece for a 1x1-then-resize Steam UI may depend on it.
3. **`XRRGetOutputInfo Workaround: initialized with override: 1 real: (nil)`** — a workaround *is*
   installed (`override: 1`) but `real` is null, i.e. it has no real function to override. **That
   string is not printed by anything in this repo** (grep over `tools/`, `app/src/main/assets/` finds
   only this doc): it comes from the hosted runtime's **patched `libXrandr.so.2`** (the same
   `override`/`real` pair the guest's `libXrandr` also carries, per `tools/gamescope/
   runtime-sonames.txt`). Reading that patch is the cheap next move; it is also the layer that must
   return a usable output mode for this to work.

**Not the ICD.** No `vkCreateSwapchainKHR`/`vkAcquireNextImageKHR`/`vkQueuePresentKHR` failure is
logged for `0x1200002`, and the ICD created the swapchain cleanly. §10's note that the X11 present
path was "implemented, present path unverified" (`f075cf6`) is now narrower: **the X11 path gets to a
created swapchain for the real client; the client dies before it presents from it.**

**The next step is a backtrace, not a log.** Signal 11 with no further detail has to be resolved with
a core or a debugger inside the guest session (`gdbserver`/`gdb` around the client under
`droiddeck-session`, or a core dump read with the `steam` binary). Everything above is where to look,
not what was proven.

**How to re-run this in one command** (the `SessionActivity` is not exported, so `am start` on it is
refused; this trampoline is):

```
adb shell 'am start -n com.droiddeck.launcher/.agent.AgentStartActivity --es request eyJtb2RlIjoic3RlYW0ifQ=='
```

## 12. Chasing the client's signal 11: what holds, and what does not (2026-10-10)

§11 left the client's crash unexplained, and said the next step was a backtrace. There is no `gdb`,
`strace` or core file in the guest, so a small `LD_PRELOAD` catcher was written instead: it catches the
fatal signal in-process and writes the registers, a stack scan and `/proc/self/maps`. **Its results are
recorded here with their caveats, because the catcher turned out to be able to cause the very fault it
was measuring.**

**What holds.**

* **The crash reproduces 4/4** on the same build: sessions 44, 45, 47 and 60 all end
  `== steam exited rc=139`, always after the client makes its own 1x1 X11 surface (`xid 0x1200002`).
  Session 60 was a plain Steam session armed only by an `ld.so.preload` line, so this is not an
  artifact of how the session was started.
* **The faulting process in the shimmed run was `steamsysinfo`**, by `/proc/self/comm` and by the
  `argv0` the report recorded:
  `/root/.local/share/Steam/steamrtarm64/steamsysinfo -steamid 0 -buildid 1791592380 -query 1
  -out-file /tmp/steamHCIgnk`. That is Steam's own GPU-topology probe — the same one that answers
  `PowerVR D-Series DXT-48-1536 MC1` through the ICD when it works. The client spawns it during UI
  bring-up, so a helper dying here is on the path to the UI coming up.
* **The fault was an instruction fetch, not a load.** `pc == si_addr`, `si_addr` in no mapping, and
  `x17` (AArch64's PLT scratch register) holding the same bad address; `lr` was in the device's bionic
  `/system/lib64/libc++.so`. So a jump went to an unmapped address.

**What does not hold.**

* **The catcher changes the thing it measures.** Preloaded into every descendant, it made
  `steamsysinfo` die where it otherwise succeeds: in §10-11's sessions `steamsysinfo -query 1` exits 0
  and prints the GPU topology, and under the catcher it faulted. The cause is the catcher itself —
  Steam's helpers take faults on purpose and recover, and a `SIGSEGV` handler turns a recovered fault
  into a kill. The handler is now gated on `comm == "steam"`, and the report names the process so a
  helper's death is never read as the client's.
* **Three shimmed runs gave three different answers** (a `libc++`/`steamsysinfo` instruction fetch;
  then no report at all; then no report at all). A report that cannot be reproduced is not evidence,
  and none of these are used as the explanation of the client's crash.
* **The `steam` process itself never produced a report**, even in the run where the catcher was armed
  through `ld.so.preload` and the client did crash (`rc=139`, session 60). The run-mode sessions were
  stopped during Steam's `Verifying all executable checksums` — about two minutes of heavy CPU on a
  fresh client — which is *before* the point where the crash happens, so those runs may never have
  reached it. Why the preload-armed session 60 produced no report is not known.

**One lead worth keeping, not yet tested.** The `XRRGetOutputInfo() is not available` line appears
exactly when the client reaches its own window and never in §10's session, which spent its whole life
downloading the 666 MB first-run update. Whatever asks for RandR output info is on the UI bring-up
path, and the guest's Xwayland does not answer it. Whether that is what the client then jumps off is
**not established** — the instruction fetch had `lr` in bionic `libc++`, not in an X library, which
argues against a direct X callback.

**What would settle it** (in order, cheapest first):
1. Give a shimmed session **three to four minutes past the client's launch** rather than stopping it
   during the checksum verify, and read `/tmp/crash.txt` then. The catcher is comm-gated to `steam`
   now, so a helper's fault can no longer be mistaken for the client's.
2. `strace -f` around `droiddeck-session` if a static strace can be dropped in the guest — the last
   syscalls before the fault would place it without needing a symbol.
3. Install a cross-built `gdb` into the runtime image and attach to the client at the moment of the
   fault.

## 13. Two things tidied while sifting (2026-10-10)

**The ICD's per-call tracing is off unless asked for.** gamescope probes a DRM modifier once per output
format per frame, and the one-line `fprintf` on that path put hundreds of thousands of lines into the
guest's `session.log` — enough to bury the guest's own errors, which is part of why §11's cause took so
long to see. The retry and the `physical_device_drm` line now print only when `PVR_ICD_DEBUG` is set in
the guest's environment; the fixes themselves are silent, since a retry that succeeds is the expected
path now. `RTLD_NOW` is also no longer redefined when the host headers already define it.

**The driver asset is now reproducible.** `app/src/main/assets/linux_driver/powervr-g5x.tzst` is built
by `docs/development/pvr-re/hybshim/package_asset.py` with a fixed mtime, uid/gid and mode and a fixed
entry order, so a rebuild from unchanged inputs is byte-identical. The ICD inside the shipped asset
(`9bcabaf73caf5fc30c5cd38da7de0204ba25b3a1872f0f2298fa5b53975b16b8`) can therefore be compared against
a fresh `build_icd.sh` output with sha256, which is how the "was the new apk actually shipped" question
gets answered.

## 14. The line every session printed, and what it cost to ignore (2026-10-10)

`MESA: error: zink: failed drmPrimeFDToHandle Invalid argument` appears in **every** session's log, from
the first one in §10 onward. §10 called it "the first suspect" and then never chased it, because the
client carried on past it and looked healthy. It is a real defect, and chasing it turned up a second
one that matters more.

**What it is.** Steam's client renders through **mesa zink** (`pEngineName: mesa zink`, in every
session), i.e. GL on top of Vulkan. Under Xwayland its GLX/DRI3 path goes
`dri_gbm -> dri2_query_image -> zink_bo_get_kms_handle -> drmPrimeFDToHandle`, and that last is a **GEM**
ioctl. `drm-gem(7)`: *"a driver that does not support gem will return ENODEV for all these ioctls."*
The render node on this device is `pvrsrvkm` (`OF_COMPATIBLE "img,gpu"`) - no GEM. Measured directly on
the device, with the node open:

```
PRIME_FD_TO_HANDLE on /dev/dri/renderD128: rc=-1 errno=22 (Invalid argument)
```

`EINVAL` is the `Invalid argument` in that log line. This is the same failure the project already fixed
once for a different GPU family: `tools/linuxfs/preload/drm.c` exists precisely to stand in for GEM
handles on a node that has none, and its header says so ("Compositors and Mesa validate or convert
dma-bufs with PRIME ioctls on the render node; KGSL has no GEM"). **It was keyed to KGSL alone.**
`/dev/kgsl-3d0` does not exist here, so `is_kgsl()` returned false for every fd and the emulation was
dead code on this device.

**The fix is a predicate, not new logic.** The rule is now the driver's own answer rather than the
node's name: the PRIME calls are sent to the real ioctl first and answered here when it fails with
`ENODEV`/`EINVAL`/`ENOTTY`/`EOPNOTSUPP`. A node that has a GEM and refuses one unknown handle also
answers `EINVAL`, and standing in for it is safe for the same reason the shim exists at all - a handle
is only a token to these callers. KGSL keeps its name-based path, so Adreno behaviour is untouched.
Everything downstream (`drmPrimeFDToHandle`, `drmPrimeHandleToFD`, `drmCloseBufferHandle`, and the
`GEM_CLOSE` interposer in `netif.c`, which already routes through `bl_drm_gem_close`) works once the
predicate matches.

**Measured:** 26 of those errors before, **0** after. **The client still segfaults.** This was a real
defect and it was *not* the crash - stated plainly, because the earlier hypothesis that it was the
crash is falsified here.

## 15. A bundled driver that could never be updated (2026-10-10, measured)

Found while checking the fix above had actually reached the device, by counting the new ICD's log lines
in a fresh session: they were still there, thousands of them. The guest was running the ICD from
whenever the driver had first been extracted, not the one in the apk just installed.

`LinuxVulkanDriverManager.installBundled`:

```java
if (new File(dir, META_NAME).isFile() && isInstalled(id)) return id;   // never re-extracts
```

A complete install is trusted forever. The method's own doc comment promised the opposite - *"A driver
whose files are incomplete ... is re-extracted, **which is what an app update relies on**"* - so this
was a bug against stated intent, not a missing feature. Since the bundled driver **is** the shim every
session runs on, the effect is that an app update could never ship a new ICD: exactly the "the new apk
was not shipped" trap, hit silently.

The extracted `meta.json` now records `installedByVersionCode`, and a complete install is trusted only
when that is the apk asking (`builtByThisApk`). An install from before the stamp existed reads as "not
this apk" and is re-extracted once, which converges and cannot leave a stale build behind.

**Measured after installing 0.3.3:** the guest's ICD log spam went from thousands of lines to **0** -
the new library is the one in use. This is the fix that makes every future ICD change testable.

**Method note, for whoever picks this up.** Both defects were in **shared components keyed to a GPU
family** (`tools/linuxfs/preload/*.c`, and the driver manager's "installed" test), not in the Steam
client, which was only the symptom carrier. The signal for the first was in every log from the start and
was deprioritised for looking non-fatal; the signal for the second was a log line that should have
disappeared and did not. When a shim's precondition is a property of the GPU family, adding a family
means auditing every shim keyed to the old one - and when a change is supposed to alter what the guest
logs, count the lines rather than trusting the install.

## 16. The crash, located: a write to a protected page inside the libhybris linker (2026-10-10)

§11-12 could not name the fault. It is named now, and it is **not in Steam, X11 or zink** - it is in
**our own bionic bridge**.

**How it was caught.** The guest has no `gdb` or `strace`, and Android routes cores to debuggerd, so a
small `LD_PRELOAD` catcher was written that interposes `sigaction()` itself: the client installs
breakpad's handler about a second after start, which replaces any handler an LD_PRELOAD constructor had
put in place, so the interposer has to wrap that later call rather than race it. It was validated first
against a five-line program that installs its own SIGSEGV handler and then crashes - the report came out
of that, so the mechanism was known to work before a session was spent on it. `LD_PRELOAD` is the
delivery route because `SessionFiles` rewrites `/etc/ld.so.preload` at every session start.

**The measurement.** `comm=steam`, 127 KB report. Resolved against `/proc/self/maps`:

```
si_addr 0x7b582dc028  [anon:linker_alloc] r--p   <- the bad access, a READ-ONLY page
pc      0x7b583ac17c  q.so (the emulated bionic linker)
lr      0x7b583ac128  q.so
x19     0x7b582dc010  [anon:linker_alloc] r--p
```

`si_addr == x19 + 0x18`, and the faulting word is `f9000e75` = **`str x21, [x19, #0x18]`**. So it is a
**store into a read-only linker page**. The same region holds the GOT slot `x16` uses for the call, and
the call's target sits between `init_default_namespaces` (q.so `0x5b430`) and `get_exported_namespace`
(`0x5c7ac`) - i.e. **inside `init_default_namespaces`**. The stack passes through `libhybris.so`,
**`libvulkan_powervr.so` (our ICD)** and `libvulkan.so.1.4.357` (the loader), so the path is the guest
Vulkan loader driving our ICD, which drives the bionic HAL through libhybris - exactly the seam this
project built.

**What that means.** This is bionic's `ProtectedDataGuard` machinery, which mprotects the linker's own
allocators and restores them on scope exit:

```cpp
void ProtectedDataGuard::protect_data(int protection) {
  g_soinfo_allocator.protect_all(protection);
  g_namespace_allocator.protect_all(protection);        // <- the page this write lands in
  g_namespace_list_allocator.protect_all(protection);
}
```

and libhybris's own `linker_block_allocator.h` says it *"provides mprotect services to the user."* So a
namespace object is being written while the allocator's pages are read-only. **The bionic linker's
self-protection is not holding under the libhybris emulation** - the guard is either not active where
this write happens, or its nesting is unbalanced, or the allocator's `protect_all` does not cover the
region the namespace was actually allocated from. Which of those it is has **not** been established; what
is established is the address, the instruction, the function and the layer.

**Where the next move is.** It is a libhybris-side fix, in the emulated linker's protection handling -
not in the ICD, not in gamescope, not in Steam. The build recipe and the patches already carried are in
`docs/development/pvr-re/hybshim/README.md`, and `build_hybris.sh` rebuilds both DSOs with zig, so this
is reachable without an Android SDK.

**Also kept:** the interposer itself (`crashcatch2.so` in this session's scratch work) is the only
debugger this guest has had, and it is what turned "it segfaults" into an address, an instruction and a
function. It is worth keeping for any future fault in the guest.

## 17. Why the crash happens: the linker's own pages are read-only when it writes to them (2026-10-10)

§16 named the layer and got two things wrong. Both are corrected here; what follows is measured by
**direct correlation within a single run**, not inferred from disassembly.

**Method.** Two `LD_PRELOAD` shims loaded together into the client: the `sigaction`-interposing crash
catcher, and an `mprotect` interposer that logs every call with its range, requested protection, and
the caller's return address (captured as the interposer's *first* instruction - reading `x30` after
`dlsym`/`open` returns garbage, which is what made an earlier attempt report an unmapped caller). One
run then yields the fault and the history of the page it faults on.

**The correlation, from one run:**

```
si_addr = 0x7b2998c028            (the store that faults)
  mprotect addr=0x7b2998c000 len=0x64000 prot=3  caller=q.so+0x63f7c   (make RW)
  mprotect addr=0x7b2998c000 len=0x64000 prot=1  caller=q.so+0x63f7c   (make READ-ONLY)
```

`prot=1` is `PROT_READ`. `q.so+0x63f7c` is the `mprotect` inside `LinkerBlockAllocator::protect_all`
(`0x63f54`), which is called 11 times - all from `ProtectedDataGuard`'s constructor, `protect_data`
and destructor. The page is a linker block-allocator page: `[anon:linker_alloc]`, and
`kAllocateSize = PAGE_SIZE * 100` (`linker_block_allocator.cpp:40`) = `0x64000` at the guest's 4096
byte pages, which is exactly the range protected.

**So the cause is:** `str x21, [x19, #0x18]` inside **`parse_LD_LIBRARY_PATH`** stores into
`g_default_namespace`'s search-path list, and that namespace object lives on a linker block-allocator
page that `ProtectedDataGuard` has put in **read-only** state. The linker is writing to its own
protected data. The invariant upstream relies on is the guard's: the pages are
`PROT_READ | PROT_WRITE` while a guard is in scope and `PROT_READ` outside it (`ref_count_` 0 -> 1
un-protects, 1 -> 0 re-protects). Here the write happens while they are `PROT_READ`, so either no
guard covers this path or `ref_count_` is unbalanced and a destructor re-protected them early.

**Two corrections to §16**, both my errors:

* the function is **`parse_LD_LIBRARY_PATH`**, not `init_default_namespaces`. §16's offset came from
  the section-table delta; the runtime map (`r-xp 00042000`) is what resolves it, giving vaddr
  `0x6c17c` rather than `0x5c17c`.
* **`ProtectedDataGuard` is live, not dead code.** §16 concluded "0 call sites, never runs" from a
  `grep` for a direct `bl` to `protect_all`'s address; the calls go through the **PLT**
  (`protect_all@plt` at `0xc5330`), which that grep did not match. The correlation above shows the
  guard ran and did protect the page.

**Why it is intermittent.** The protection is a state, not a constant: it depends on which guards are
in scope when `android_linker_init` runs. That matches what was seen - most sessions reach the
client's window and die there, and one run produced the crash with the log showing both the RW and the
read-only call on the same page.

**What is still not established:** which of the two - a missing guard around this path, or an
unbalanced `ref_count_` - it is. That is the next measurement, and it is a libhybris-side change either
way (`docs/development/pvr-re/hybshim/README.md`, rebuilt by `build_hybris.sh`), not an ICD, gamescope
or Steam change.

## 18. The missing guard, fixed - and the fault after it (2026-10-10)

§17 asked which of two it was. It is the first, measured: **`ProtectedDataGuard::ref_count_` is 0 at the
fault**, so no guard is in scope, and the store lands on a page the guard has left `PROT_READ`. The
source says the same thing:

```cpp
static ElfW(Addr) linker_main(...) {
  ProtectedDataGuard guard;          // present

extern "C" void android_linker_init(...) {
                                     // absent
  parse_LD_LIBRARY_PATH(ldpath_env);  // stores into g_default_namespace's search-path list
  parse_LD_PRELOAD(ldpreload_env);
  add_vdso();                         // allocates into the block allocator
  init_default_namespaces(...);
```

`android_linker_init` is libhybris's own entry point - the one a session actually takes - and it writes
linker data without the guard that `linker_main` has. That is the whole bug.

**The fix** is the guard, and it is carried as
`docs/development/pvr-re/hybshim/patches/0001-android-linker-init-needs-the-protected-data-guard.patch`.

**It works.** With the patched `q.so` deployed the old fault is gone - `si_addr` is no longer on a
protected linker page, and the instruction-fetch-through-`x16` into that page with it.

**A different crash follows it, at the HAL boundary.** The next fault is an **instruction fetch to
unmapped memory**:

```
si_addr == pc == 0x71bc2fbd9c     (in no mapping - a gap between q.so's data and the ICD's)
lr      -> /vendor/lib64/hw/vulkan.powervr.so  +0x151f10
x16     -> /vendor/lib64/hw/vulkan.powervr.so  +0x26cf8
```

So the call goes into the PowerVR HAL (`lr` and the `x16` GOT slot are both inside it) and lands on
nothing. Different layer entirely: a call made into the bionic HAL through the libhybris bridge whose
target is not mapped. It is **not** characterized further - it needs the vendor blob's symbols, which
the app cannot read, and it is a separate measurement.

**Provenance caveat on the fix test:** the patched `q.so` was copied into the app's extracted driver
directory for the test. That directory is rebuilt from the apk's asset whenever the version stamp
changes, so the result stands as a measurement but the patch still has to be built into the shipped
asset - i.e. into `q.so` as `build_hybris.sh` produces it - for a session to pick it up on its own.

## 19. The fault after the guard fix, symbolized (2026-10-10)

The guard patch (§18) removes the protected-page store. The fault that follows is a **jump through a
bad PLT slot in the bionic HAL**, and it is now symbolized to the instruction. The vendor blob is
world-readable (`/vendor/lib64/hw/vulkan.powervr.so`, 3167784 bytes), so it can be resolved offline;
`lr` and `x16` from the report land at file offsets `0x1b9f10` and `0x286cf8`.

```
vulkan_device_open (HAL, file 0x1b9ee0):
  1b9f0c: bl  0x25c9a0 <strcmp@plt>        ; lr = 0x1b9f10, matches the report
strcmp@plt:
  25c9a0: adrp x16, 0x286000
  25c9a4: ldr  x17, [x16, #0xcf8]          ; x17 = GOT[strcmp]
  25c9ac: br   x17                         ; -> 0x71bc2fbd9c, unmapped
```

`0x286cf8` is confirmed by the relocation table to be the `R_AARCH64_JUMP_SLOT strcmp@LIBC` slot. The
value in it, `0x71bc2fbd9c`, is in **no mapping**: it sits in the gap between `q.so`'s data end
(`0x71bc2ae000`) and this ICD's first mapping (`0x71bc310000`). Bionic's `strcmp` is in
`/apex/com.android.runtime/lib64/bionic/libc.so` at `0x71a5...`, nowhere near it.

So the HAL's `strcmp` PLT slot does not hold a usable `strcmp`. It reads like an unresolved or wrongly
resolved slot - a PLT slot whose initial value should be the lazy-binding resolver, or a resolved
target, and is neither.

**This is the same class as §17-18**: the HAL is a bionic library, so its relocations and its PLT
resolution are done by `q.so`'s linker, and that is where the last two faults have lived. Not the ICD,
not the shim's own logic, not Steam.

**Not established:** whether the slot was never resolved (lazy binding not entered) or was resolved to
the wrong address. Distinguishing them is the next measurement, and the interposer used for §17 can
answer it by logging the resolver entry for this slot.

## 20. Characterization: the HAL's PLT is not one stuck slot, it is a set (2026-10-10)

§19 left one question: is the bad `strcmp` slot a single slot, or is PLT resolution broken generally?
Measured - and it is neither "one" nor "all", which matters for what to do next.

**Method.** With the guard fix in place (§18), the crash catcher dumped the whole of the HAL's
`.got.plt` at fault time: file offset `0x286ab0`, `0xb00` bytes, 352 slots (the HAL's base comes from
`x30 - 0x1b9f10`, since the fault's link register is inside `vulkan_device_open`). Every slot was then
resolved against `/proc/self/maps` from the same report.

**Result:**

* **17 of 352 slots hold an address in no mapping.**
* Their targets are all in one ~6 KB span, `0x76f5b2bd5c`-`0x76f5b2d700`, which is a real **hole**
  between `q.so`'s last mapping (`0x76f5adf000`-`0x76f5ae2000`) and this ICD's first
  (`0x76f5b40000`).
* The symbols behind them are a coherent set:

  ```
  malloc, memcpy, strlen, strcmp, fclose, fgets, fseek, ftell,
  pthread_mutex_lock, pthread_mutex_unlock, pthread_cond_wait,
  pthread_cond_signal, pthread_cond_broadcast,
  pthread_rwlock_rdlock, pthread_rwlock_wrlock, pthread_rwlock_unlock,
  pthread_getspecific
  ```

* Neighbouring slots resolve correctly - 3, 8, 15, 69, 72, 83 land in glibc's `libc.so.6` and
  `libufwriter.so` - so the whole table is not bad, and this is not a base-miscalculation artefact.

**Why this shape.** Those 17 are exactly the class of libc function libhybris **interposes** rather than
passes through: the memory allocator and the pthread primitives are the ones its `libhybris.so` hook
layer replaces so bionic code can run on glibc. The addresses point into a region that was never
mapped, so the mechanism is that the HAL's imports of these were resolved to somewhere that is not
there. Which of {resolved to the wrong address, resolved but the region was unmapped afterwards, never
resolved} it is has **not** been established, and that is the next measurement.

**What this changes.** Not one slot - so not a small patch. Not the whole table - so not "the linker
cannot do PLT at all", which would have been the worst answer. It is a bounded set of interposed libc
symbols, which is a concrete thing to chase rather than a mystery.

**Also a correction:** the catcher's `q.so_base` readout assumes the faulting `pc` is at `q.so+0x6c17c`,
which is only true of the §17 crash. In this report it printed a meaningless value; only `si_addr`,
`pc`, `lr` and the register set are load-bearing there.

## 21. Where the 17 bad slots point, and the diagnostic that is silent (2026-10-10)

§20 established that 17 of the HAL's 352 PLT slots hold unmapped addresses, and that they are the
allocator and pthread symbols libhybris interposes. Two more facts, and one dead end worth recording so
nobody repeats it.

**Where they point.** With `q.so`'s real base (`0x76f59f0000`, its `r--p` mapping at file offset 0) the
17 addresses are `base + 0x13bd5c` through `base + 0x13d700`. `q.so`'s loaded image ends at
`base + 0xf1408` (the last `LOAD` is `vaddr 0xee2a8`, `memsz 0x3160`). So **they point past the end of
the DSO's own image**, about 480 KB beyond it, into a region that was never mapped. They are not
arbitrary: they are tightly packed (about `0x30` apart across a 6 KB span), which is the shape of a
table of same-sized entries rather than 17 independent function addresses.

**The diagnostic exists and is silent.** `__hybris_get_hooked_symbol` knows about exactly this failure
and says so in a comment:

```c
if (strncmp(sym, "pthread", 7) == 0 || strncmp(sym, "__pthread", 9) == 0) {
    if (strcmp(sym, "pthread_sigmask") == 0) return NULL;
    counter--;
    // If you're experiencing a crash later on check the address of the
    // function pointer being call. If it matches the printed counter value
    // here then you can easily find out which symbol is missing.
    LOGD("Missing hook for pthread symbol %s (counter %" PRIiPTR ")\n", sym, counter);
    return (void *) counter;
}
```

It returns a **small integer as a function pointer** when a pthread hook is missing - the crash it
describes is this crash. The log it writes is at `HYBRIS_LOG_DEBUG`, and `logging.c` defaults
`hybris_minimum_log_level` to `HYBRIS_LOG_WARN`, so none of it reaches the session log.

**The dead end.** `HYBRIS_LOGGING_LEVEL=debug` is the switch (`logging.c:45`), and setting it - both on
the client directly and on the session script so every guest process gets it - produced **no**
`Missing hook` and no `Could not find a hook` lines, in a session that still crashed. So either the
level is not being honoured on this path, or the hooks are being resolved by a different route than
`__hybris_get_hooked_symbol`'s missing-hook branches. Raising the level is worth trying again with the
output captured separately (the guest's `stderr` is interleaved into a very noisy session log), but it
did not answer the question here.

**Where this leaves it.** The mechanism is still open: whether those slots were resolved to a table that
was never mapped, or resolved and then unmapped. What is settled is the *shape* - a bounded set of
interposed libc symbols, all pointing at the same unmapped region just past `q.so`, and a resolver that
has a branch for exactly this and does not report it.

## 22. The ENOSYS sessions were a setting, not damage (2026-10-10)

Sessions 102-114 failed early with `Function not implemented` (ENOSYS) on `getcwd`, `mkdir /root`,
`python3` and `std::filesystem::status`, and gamescope aborted with
`filesystem_error: status: Function not implemented [/etc/gamescope/scripts]`. The **desktop (lxqt)
mode failed the same way**, which made it look like a device-wide regression. It was not: it was one
setting.

**Cause.** `shared_prefs/session.xml` had `prootNoSeccomp = true`, written at 20:21. That makes
`ProotFastPath.enabled()` false -

```kotlin
fun enabled(context: Context): Boolean =
    SessionPrefs.prootFastPath(context) && !SessionPrefs.prootNoSeccomp(context)
```

- so `SessionFiles` stops writing `/usr/local/lib/libblfastpath.so` into `/etc/ld.so.preload`, and
  without the fast path this kernel's filesystem syscalls come back ENOSYS. Turning the setting off
  (`prootNoSeccomp = false`) restores the preload line and **ENOSYS goes to 0**; the session then runs
  normally into gamescope, Xwayland and Steam's update.

**The app already knows this failure by name** - `strings.xml`:

> *The log shows %1$d "Function not implemented" error: proot's seccomp acceleration is failing a
> helper on this device. Try ... → "Run proot without seccomp" ... and start again.*

and `SessionActivity.sessionEndHint` detects the storm (8 or more lines) and offers the switch. Note
the two switches point opposite ways: **"Run proot without seccomp"** *causes* this, while **"Skip
Steam's xalia helper"** is the fix that is meant to answer it.

**Correction to the temptation to blame the debugging.** The rebuilt `libhybris.so` and `q.so` were
byte-identical to the project's `hybshim/dist/` copies for the non-DEBUG build, and restoring those
originals did not change the ENOSYS count - it tracked the pref flip, not the libraries. The earlier
"hand-copied a file in and broke the runtime" reading was wrong; the correlation was with the setting
being written, not with anything under `files/linuxfs/`.

**Still true and worth keeping:** `droiddeck-session` and the other `usr/local/bin/*` scripts are
redeployed from the apk at every session start (`SessionFiles.kt`), so a guest-side edit to one lasts
exactly one session. Instrumentation that must survive has to go through the app build.

## 23. The guard fix was never shipped - and the rig proved it in one line (2026-10-10)

With the rig live, the crashed page resolved to a **read-only linker page**, i.e. fault #1, the
`ProtectedDataGuard` bug that was fixed (§18). It could not be: a fix did not change the fault, so the
fix was not running.

**It was not running because it had never been shipped.** The guest's `q.so` was `fb3908ce` - the
original - while the fix existed only as a patch in the repo and as a file that had been hand-copied
once. That is the "the new apk was not shipped" trap, and the rig turned it from an inference into one
`md5sum`.

Getting it shipped needed two things, and the second was the real defect:

* the built `q.so` had to be put into the shipped asset (`package_asset.py`), and the apk's version
  bumped so the driver is re-extracted;
* **`SessionService.effectiveLinuxDriver` returned early when a driver was already chosen**:

  ```kotlin
  private fun effectiveLinuxDriver(chosen: String): String {
      if (!chosen.isEmpty()) return chosen          // <- never reaches installBundled
      ...
      return LinuxVulkanDriverManager(this).installBundled(id) ?: ""
  }
  ```

  `session.xml` holds `<string name="linuxDriver">powervr-g5x</string>`, so the choice is always set on
  this device and `installBundled` was **never called from a session**. A bundled driver that is
  explicitly chosen could never be updated - the version stamp of §15 was correct and unreachable.

  Now the install runs first and the choice is returned after it. `installBundled` re-extracts only
  when this apk did not put the directory there, so an unchanged driver still does no work.

**Measured after the fix:** the guest extracts `q.so` = `de7e96cd...` (the patched build) with
`installedByVersionCode: 14`, and the session's fault **changed** -

```
before  si_addr=...c028  pc=<libhybris+0x2d9c>   a STORE into a read-only linker page
after   si_addr == pc == <ICD + 0x4d9c>          an INSTRUCTION FETCH, lr in the HAL's
                                                 vulkan_device_open (HAL file 0x1b9f10)
```

The store-into-a-protected-page fault is gone. What remains is a jump to an address in a `---p`
(PROT_NONE) region inside **our own ICD**, called from the HAL's `vulkan_device_open`.

**Also found:** installing an apk does not restart a running app process, so the new code is not in
effect until it is force-stopped - and the driver refresh does not happen either, because it runs in
the old process. This cost a run; `am force-stop com.droiddeck.launcher` is now part of the loop.

See `docs/development/pvr-re/rig/` for the instrumentation that found all of this.

## 24. What the remaining fault is, and why I stopped short of the reading (2026-10-10)

After the guard fix actually shipped (§23) the fault changed to an **instruction fetch**, `si_addr == pc`,
with `lr` inside the HAL's `vulkan_device_open` (HAL file offset `0x1b9f10`). The address is in a
`---p` (PROT_NONE) region, and the protection is a startup map with a **zero file offset** - so the
value that was jumped through was present at load, before anything could disturb it. It is not a
region that was unmapped later; it never had a way to be executed.

**The one reading that fits, and what is wrong with it.** An address inside the ICD's own mapped image
is what a **PLT slot with no relocation applied** holds: a slot the loader never filled still contains
whatever the file has there, and the file is mostly zeros, so a slot at file offset X points at
`icd_base + X`. That would mean the HAL's imports of the interposed libc/pthread symbols are not being
relocated - the same family as §20's 17 slots.

**It is NOT established, and one measurement contradicts it.** In §20 the 17 slots resolved correctly
into `libhybris.so` at the HAL's `open`. If the HAL's imports are handled, that reading is wrong here.
I also do not have the PLT values for *this* run: the crash report's GOT dump is invalid, because
`crashcatch` computed the HAL's base as `pc - 0x6c17c` - a constant from when the fault was in q.so.
With the fault now in the ICD, that expression produced a nonsense base and the dump read the wrong
memory. `crashcatch.c` is fixed at source (it takes each module's base from `/proc/self/maps` now) but
**a rebuilt shim could not be produced**: WSL stopped starting partway through this session, and all
cross-compilation goes through it.

So: the remaining fault is an instruction fetch through a value that was in place at load, aimed inside
the ICD's image, called from the HAL - and the next step is to re-run the rig to get PLT values taken
with a base that is trustworthy. Until then the reading above is a hypothesis, not a result.

## 25. Pinned: the HAL's interposed-symbol PLT slots point into the ICD (2026-10-10)

With `crashcatch` fixed to take each module's base from `/proc/self/maps` (it had used `pc - 0x6c17c`, a
constant from when the fault was in q.so, and read the wrong memory once the fault moved), the HAL's
PLT was dumped with trustworthy bases, three runs in a row:

```
run 3  ICD base 0x7618680000   slot 73 value - ICD = 0xbd9c
run 4  ICD base 0x73ac190000   si_addr        - ICD = 0xbd9c   (no PLT dump - maps parse short-read)
run 5  ICD base 0x7b9cad0000   slot 73 value - ICD = 0xbd9c
```

**Exactly 17 of the HAL's 352 PLT slots hold an address inside our ICD** (base `0x7b9cad0000`):

```
slot  13 pthread_mutex_lock   -> ICD+0xbf20      slot 167 fclose   -> ICD+0xd00c
slot  14 pthread_mutex_unlock -> ICD+0xbfdc      slot 168 fgets    -> ICD+0xd2cc
slot  30 malloc               -> ICD+0xbcec      slot 197 fseek    -> ICD+0xd5c4
slot  57 pthread_cond_wait    -> ICD+0xc544      slot 198 ftell    -> ICD+0xd700
slot  68 pthread_cond_signal  -> ICD+0xc448      slot 220 pthread_getspecific -> ICD+0xca8c
slot  70 pthread_cond_broadcast-> ICD+0xc34c     slot 348 strlen   -> ICD+0xbd8c
slot  73 strcmp               -> ICD+0xbd9c      slot 349 memcpy   -> ICD+0xbd5c
slot  75/76/77 pthread_rwlock_{w,un,rd}lock -> ICD+0xccfc / +0xccbc / +0xcd64
```

Every one is in `ICD+0xbd5c .. 0xd700` - inside **our ICD's PROT_NONE inter-segment gap** (vaddr
`0x64f8..0x164f8`, which is `---p` by design between the read-only and executable segments). Slot 72,
a neighbour of the interposed set, resolves normally to a real module (`0x7baa17db00`), so the table is
not globally broken.

**So:** the HAL's imports of the **allocator and pthread primitives - the symbols libhybris
interposes** - resolve to addresses in our ICD's PROT_NONE gap, and the first call through one of them
(`strcmp`, from `vulkan_device_open`) is the fault.

**Not yet established: why.** The shape is a table indexed consistently by slot, and the values are
frame-independent (the same 17 slots, the same offsets, across every run), so something is filling them
deliberately rather than corrupting them at random. Candidates not yet tested: libhybris's hook layer
returning a table entry that lands in the ICD's address space; the ICD's own symbol table being used as
the resolution target for interposed symbols; or an offset applied to the wrong module's base. The
`/proc/self/maps` short-read is now fixed in `crashcatch.c` (procfs answers large reads in chunks, so a
single `read()` got only the first few mappings).

**Gap in the rig:** `symwatch` produced no output (`/tmp/sym.log` absent) - libhybris's hook callback is
a single slot and the ICD sets its own, so whichever runs last wins. To see what the hook layer is
asked, the ICD's callback has to chain to the recorder, or the ICD's own trace has to be used.

## 26. The PLT values do not change - and they are not in the ICD (2026-10-10/11)

Two corrections to §25, both from measuring rather than reasoning, and the finding that survives.

**Correction 1: the values are not in our ICD.** §25 read "ICD+0xbd9c" because one run's layout put the
ICD where those addresses fell. With the ICD dumping its own view at the HAL's `open`
(`PVR_ICD_DIAG`), the target resolves against the maps to an **anonymous `---p` (PROT_NONE) region**,
not to `libvulkan_powervr.so`. The "ICD + 0xbd9c" was a coincidence of that run's address layout.

**Correction 2: the slots do not change.** Same pid, same process, before and after the HAL's `open`,
and then at the crash:

```
OPEN  [before_open] pid=29299 slot73 = 0x7d4843bd9c
OPEN  [after_open]  pid=29299 slot73 = 0x7d4843bd9c
CRASH               pid=29299 si_addr = 0x7d4843bd9c
```

So nothing rewrites them. **They are already wrong when the HAL loads**, which makes this a
**resolution** error and not corruption - and it rules out "something disturbs the table later".

**What survives, and is now measured:**

* The HAL's 17 interposed-symbol PLT slots - `malloc`, `memcpy`, `strcmp`, `strlen`, `fclose`,
  `fgets`, `fseek`, `ftell`, `pthread_mutex_lock/unlock`, `pthread_cond_wait/signal/broadcast`,
  `pthread_rwlock_rdlock/wrlock/unlock`, `pthread_getspecific` - hold addresses in **one anonymous
  PROT_NONE region**, tightly spaced (~`0x2e0` apart over ~6.7 KB).
* The values are identical before and after `open`, and identical across every run (same offsets
  relative to whatever base they share).
* The crash is the first call through one of them: `strcmp`, from the HAL's `vulkan_device_open`
  (HAL file offset `0x1b9f10`).
* Neighbouring non-interposed slots resolve to real modules, so the table is not globally broken.

**So the defect is: the HAL's imports of exactly the symbols libhybris interposes are resolved to an
anonymous PROT_NONE region instead of to libhybris's hook functions.** The region's identity - what
allocated it, and why the hook layer hands out addresses inside it - is the next question.

**Rig note:** the ICD now takes `PVR_ICD_DIAG` from the session environment (set by the overlay session
script's rig block) and appends the HAL's PLT at `open` to that file, so open-time and crash-time views
can be compared in one session. That comparison is what produced both corrections above.

## 27. The mechanism: libhybris resolves the HAL's interposed imports to trampolines that are not
## executable (2026-10-11)

The ICD now logs every symbol libhybris is asked to resolve (its hook callback, registered before the
HAL is loaded, writing to `PVR_ICD_DIAG`). One session produced **109,590 queries**, and among them the
HAL's own, matching the bad slots exactly:

```
ICD hook sym=strcmp            requester=/vendor/lib64/hw/vulkan.powervr.so
ICD hook sym=malloc            requester=/vendor/lib64/hw/vulkan.powervr.so
ICD hook sym=pthread_mutex_lock requester=/vendor/lib64/hw/vulkan.powervr.so
ICD hook sym=fclose / pthread_getspecific / strlen / memcpy / __memcpy_chk   (same requester)
```

So libhybris **is** asked, for exactly the symbols that end up unusable - and what it answers becomes
the HAL's PLT slot.

**What it answers.** `hybris/common/wrappers.c:157`:

```c
wrapper_addr = mmap(NULL, wrapper_size,
    PROT_READ | PROT_WRITE | PROT_EXEC,
    MAP_ANONYMOUS | MAP_PRIVATE, 0, 0);
...
memcpy(wrapper_addr, wrapper_code, wrapper_size);
return (void*)wrapper_addr + thumb_fixup;
```

For each interposed symbol libhybris can hand back a small generated **trampoline** in a fresh mapping,
rather than the hook function itself. The HAL's slots hold addresses that resolve to an **anonymous
`---p` (PROT_NONE) region** - the shape of that trampoline area, but with no access. The first call
through one of them (`strcmp`, from `vulkan_device_open`) is the fault.

**So the defect is:** the trampolines libhybris generates for the HAL's interposed imports live in a
region that is not executable, so calling through them faults immediately. It is not corruption (the
values are identical before `open`, after `open` and at the crash, §26) and not our ICD (§26).

**What is not established:** why that region is PROT_NONE. `create_wrapper` asks for
`PROT_READ|PROT_WRITE|PROT_EXEC`, and the mapping that results is read-only-none - so either the
`PROT_EXEC` request is refused or downgraded on this kernel, or the address handed out is not the
mapping that was made. `create_wrapper` only generates a trampoline when tracing is on
(`case WRAPPER_HOOKED: if(!trace_hooked) return function;`) and no `HYBRIS_TRACE_*` is set in the
session, which is a second thing that does not add up yet.

**Next step, and it is a lever as well as a measurement:** have the ICD's hook callback resolve each
symbol itself (`android_dlsym`) and return that, instead of returning NULL and letting libhybris answer
with a trampoline. That logs the address actually handed to the HAL *and* bypasses the trampoline path
if the trampolines are the problem.

## 28. Bypassing libhybris's hook answer breaks the session - a negative result (2026-10-11)

§27's next step was to have the ICD's hook callback resolve each symbol itself (`dlsym`) and return
that, on the theory that libhybris's trampoline answer is the fault. It was implemented and it is
**wrong**: the session died in **1.8 seconds**, right after gamescope loaded, with `guest.exited
status 0` and nothing further - no Wayland backend, no Xwayland, no Steam. The diagnostic file was
empty, so it died before the HAL was even opened.

That is a useful result rather than a dead end:

* **libhybris's answer to these imports is load-bearing.** Handing the HAL the plain glibc function
  instead of libhybris's own is not equivalent - bionic callers depend on libhybris's hooks for
  semantics that a direct glibc function does not provide, and breaking that breaks the whole guest,
  not just the HAL. So the fix cannot be "resolve it a different way"; whatever libhybris hands back
  has to become executable.
* It also confirms the callback is genuinely on the HAL's resolution path in every process (the
  ICD's `pvr_icd: HYBRIS_LINKER_DIR=` line was the last thing printed), which is why the logging
  version (§27) is trustworthy.

**Reverted**; the committed ICD logs the queries and returns `NULL` (libhybris decides), and a session
on it behaves as before - 914 log lines and the expected `rc=139`.

**Where this leaves the mechanism.** The HAL asks libhybris for `strcmp`/`malloc`/`strlen`/`memcpy`/
`fclose`/`pthread_*` with `requester=/vendor/lib64/hw/vulkan.powervr.so`; what libhybris returns for
those ends up in the HAL's PLT and points into an anonymous **PROT_NONE** region. Returning something
else breaks bionic, so the region must be made executable rather than avoided. Why that region is
`PROT_NONE` is still not established, and `create_wrapper`'s trampoline path is not obviously it -
it only generates a trampoline when `HYBRIS_TRACE_*` is set, which nothing in the session sets.

## 29. Where the PowerVR chase stands after the rig (2026-10-11)

A checkpoint, because the last several sessions produced a mechanism chain and several corrections
rather than a fix, and the open question has narrowed to one.

**Proven, in order:**

1. A Tensor G5 session now gets all the way to Steam's client: gamescope's Wayland backend, Xwayland,
   the ICD, the client's own window and swapchain. That is the three original blockers (§9) plus the
   linker guard (§18) fixed and shipped.
2. The client then dies on **the first call through the HAL's PLT slot for `strcmp`**, from the HAL's
   `vulkan_device_open` (HAL file offset `0x1b9f10`).
3. That slot, and the 16 others for the symbols libhybris exists to interpose
   (`malloc`, `memcpy`, `strlen`, `fclose`, `fgets`, `fseek`, `ftell`, the `pthread_mutex/cond/rwlock`
   set, `pthread_getspecific`), hold addresses in **one anonymous `---p` (PROT_NONE) region**.
4. The values are **identical before the HAL's `open`, after it, and at the crash** (same pid), so this
   is a resolution result, not later corruption.
5. libhybris's hook layer **is** asked for exactly those symbols, with
   `requester=/vendor/lib64/hw/vulkan.powervr.so` (its own hook callback, logging from the ICD, 109,590
   queries in one session).
6. Returning anything other than libhybris's answer kills the session in under two seconds, so its
   answer is load-bearing for bionic callers and the region must be made executable rather than
   resolved around.

**Corrections made along the way** (each was a claim I had to withdraw after measuring): the fault was
not in Steam's UI, not in X11, not zink, and not Steam's `strcmp`(); the guard fix was not "dead code";
the bad values are not in our ICD; the values do not change during the process; and `crashcatch`'s
bases were computed from a constant that only held while the fault was in q.so.

**The open question, and it is now one question:** what is that anonymous PROT_NONE region, and why
does libhybris's answer for an interposed symbol land in it? Candidates not yet separated:

* a libhybris hook table or trampoline area that is allocated but never made executable on this kernel;
* the HAL's relocations pointing at a per-process stub table the ICD is expected to populate;
* an address-space layout difference (32-bit-era assumptions in the linker about where scratch memory
  lives) that this device's 39-bit layout breaks.

**Next step:** log the *value* the ICD's hook callback is asked about and the value libhybris returns
for it, in the same line - the callback cannot see libhybris's own answer (it runs before it), so this
needs the linker's relocation of the HAL's PLT logged instead, which `mmwatch`-style instrumentation
cannot reach because it happens inside q.so. Attaching to the HAL's relocation directly, or building
`libhybris.so` with its `DEBUG` logging *and* a working `HYBRIS_LOGGING_LEVEL` (see the rig README for
why a plain `-DDEBUG` build breaks gamescope), is the way in.

## 30. Corrections: `-DDEBUG` is safe, and its logging is too noisy to use (2026-10-11)

Two corrections to earlier notes, both from running it rather than reasoning about it.

**`-DDEBUG` does not break gamescope.** Earlier notes (and the memory file) said an `-DDEBUG` build of
`libhybris.so` "breaks gamescope outright". That was wrong - it was **confounded with the
`prootNoSeccomp` setting**, which was on at the time and breaks every session regardless. Measured: a
`-DDEBUG` build runs a session normally through `Initted Wayland`, Xwayland and Steam, and dies at the
usual `rc=139`.

**Its logging is unusable anyway.** With `HYBRIS_LOGGING_LEVEL=debug` and
`HYBRIS_LOGGING_TARGET=/tmp/hybris.log`, one session wrote a **2.8 GB** log - every `strcmp`,
`strlen` and `malloc` the guest calls, logged per call. The device had 57 GB free so it survived, but
that is a hazard, not a tool, and the volume is dominated by hooks outside libhybris
(`AIBinder_*`, `AHardwareBuffer_*`, ...) that the session log already reports on its own.

What it did establish, cheaply: **libhybris's hook functions execute normally** -
`HOOKS .../hooks.c:2643 (int _hybris_hook_strcmp(const char *, const char *)) DEBUG: s1 'arch' s2
'arch'` - so the fault is not "the hook is broken". The HAL's PLT slot holds something else, and the
DEBUG logging does not report what a symbol resolved to, only when a hook runs.

**If a verbose build is needed again:** put `HYBRIS_LOGGING_TARGET` on the phone's external storage and
cap it, or patch `hybris_should_log` to filter to `vulkan.powervr` - the same filter the ICD's callback
already applies.

## 31. ROOT CAUSE: libhybris hands the HAL an unmapped hook address (2026-10-11)

Instrumented libhybris's own resolver (`hookret_log` at each return of
`__hybris_get_hooked_symbol`, writing `/tmp/hookret.log`, filtered to `vulkan.powervr`, no DEBUG build
needed) and compared it with the crash **in the same run, same pid**:

```
11136 strcmp /vendor/lib64/hw/vulkan.powervr.so -> 0x7e8890bf94 (hook)
comm=steam  caught signal 11  si_addr=0x7e8890bf94  pc=0x7e8890bf94
```

**`si_addr == pc == exactly the address libhybris returned.** And resolving it against the crash
report's 960 mappings:

```
strcmp hook / si_addr 0x7e8890bf94  ->  UNMAPPED
x16 (the HAL's GOT slot)            ->  inside /vendor/lib64/hw/vulkan.powervr.so  (correct)
```

So the whole chain is now closed and it is **not** our ICD, not the HAL, not Steam:

1. The HAL imports `strcmp` (and the other interposed symbols) and asks libhybris to resolve it.
2. **libhybris answers with an address that is not mapped in the process.**
3. The HAL's PLT slot is filled with that address - correctly, as far as the relocation goes.
4. The first call through it, from `vulkan_device_open`, fetches an instruction from unmapped memory and
   faults.

**What is still open, and it is now a libhybris-internal question:** why the returned address is
unmapped. `HOOK_INDIRECT(strcmp)` is defined as `{#symbol, _hybris_hook_##symbol, _hybris_hook_##symbol}`
(`hooks.c:189`), i.e. it should be the address of a real function inside libhybris - and libhybris's own
mappings are present. Candidates: the hook is a non-exported or generated entry that lives in a section
this build does not map; an ifunc resolved at load to something wrong; or a relocation applied against a
base that differs on this device's layout.

**Corrections in this section's history**, since several claims had to be withdrawn as the fault moved
and the instruments were fixed: the fault was not in Steam's UI, X11, zink, or Steam's own `strcmp()`;
the guard fix was not dead code; the values are not in our ICD; the values do not change during the
process; `crashcatch`'s bases were computed from a constant that only held while the fault was in q.so;
and `-DDEBUG` does not break gamescope.

**Where the next fix goes:** libhybris. `hookret_log` and the rig are the instruments; `build_hybris.sh`
rebuilds both DSOs with zig, so a candidate fix is one build and one session away.

## 32. The returned address is a thunk in a region that is not mapped (2026-07-11)

§31 established that libhybris returns an address that is unmapped. Narrowing where that address is
relative to:

```
crashing process (pid 11136):
  q.so base        0x7e887d0000
  libhybris base   0x7e7e520000
  ICD base         0x7e88960000
  strcmp hook      0x7e8890bf94   -> no mapping (a gap between q.so's data and the ICD)
```

The hook value's implied base is `0x7e88900000`, which is none of the three. And across processes the
values share a **fixed low offset**: every process's `strcmp` hook ends in `bf94` -

```
pid 10522  0x7e652ebf94      pid 10645  0x7c7746bf94
pid 11136  0x7e8890bf94      pid 11137  0x71d452bf94
```

The offsets of the interposed set from that base are all within ~0x100 bytes of each other
(`malloc` +0xbee4, `memcpy` +0xbf54, `strlen` +0xbf84, `strcmp` +0xbf94). Real functions are not
64 bytes apart, so these are **thunks** - small stubs, one per interposed symbol, in a single region -
and that region is not mapped in the process that calls through them.

So the defect, stated as tightly as the evidence allows: **libhybris builds a table of per-symbol thunk
addresses for the interposed imports and hands them to the HAL, and the region those thunks live in is
not mapped (or not executable) in the process, so the first call through one faults.**

**What is left, and it is one question:** where is that region meant to be, and why is it not mapped?
The address is computed from something that varies per process but is not any module's load address, so
the next step is to find the code that builds the thunk table and read the base it uses -
`hooks.c`'s hook tables (`HOOK_INDIRECT`, `hooks_common`/`hooks_p`) and whatever supplies the per-process
base are the places to look.

**Instruments that got here**, all kept: `docs/development/pvr-re/rig/` (mmwatch, symwatch, crashcatch,
build.sh, README) and, for libhybris's own resolver, `hookret_log` in `hooks.c` writing
`/tmp/hookret.log` filtered to `vulkan.powervr`. That last one is what closed the chain: it showed the
returned value and the crash address are the same number.

## 33. EXACT ROOT CAUSE: the hook address is computed against the wrong base (2026-10-11)

The returned value's low bits identify the function. From the built `libhybris.so`:

```
000000000003bee4 t _hybris_hook_malloc
000000000003bf94 t _hybris_hook_strcmp
```

`_hybris_hook_strcmp` is at file offset `0x3bf94` - and **every** returned hook value ends in `bf94`
(`0x7e652ebf94`, `0x7c7746bf94`, `0x7e8890bf94`, `0x71d452bf94`), `malloc`'s in `bee4`. So libhybris is
returning the right function, computed against the wrong base:

```
returned strcmp hook        = 0x7e8890bf94
correct _hybris_hook_strcmp = 0x7e7e56bf94      (libhybris base 0x7e7e520000 + 0x3bf94)
returned - 0x3bf94          = 0x7e888d0000      <- the base it used
```

**Off by `0xa3a0000`.** The offset within the function is exactly right; the base is not. That is why
the address lands in a gap between `q.so` and the ICD rather than in libhybris's code segment.

**So, precisely:** libhybris resolves the HAL's interposed imports (`strcmp`, `malloc`, `memcpy`,
`strlen`, `fclose`, `pthread_*`, ...) to its own hook functions, but computes their addresses against a
base that is not its own load address, and hands those to the HAL. The HAL's PLT slots receive them
correctly; the first call through one faults because the address is not mapped.

**What remains is a single libhybris-internal question:** what base is that, and why. It is not the
ICD, not `q.so` and not libhybris. `hookret_log` (§31) and `build_hybris.sh` are the instruments; the
next step is to find the expression that forms these addresses - the hook table's `func` entries and
whatever supplies the per-process base - and read it. That is a libhybris source change, not an ICD or
app one.

**The whole chain, for the record:** gamescope's DRM primary-node gate -> the linker's
`ProtectedDataGuard` -> the driver that could never be updated -> **the HAL's interposed imports
resolving to unmapped addresses, because the hook addresses are computed against the wrong base.**
Every link was measured on the device, and each was found only after several earlier readings of the
same fault were withdrawn.

## 34. The base mismatch, measured exactly (2026-07-11)

Instrumented the resolver to log the hook table entry and `dladdr`'s view of libhybris's own base. One
run, one pid:

```
15607 strcmp /vendor/lib64/hw/vulkan.powervr.so -> 0x74135ec080 (hook)
   TABLE entry=0x74136385a0 name=strcmp func=0x74135ec080 debug=0x74135ec080 libhybris_base=0x74135b0000
comm=steam  caught signal 11  si_addr=0x74135ec080  pc=0x74135ec080
```

And the same process's `/proc/self/maps`:

```
0x740d2d0000-0x740d2fa000 r--p   .../powervr-g5x/libhybris.so     <- libhybris's REAL base
0x7413502000-0x7413577000 r-xp   .../powervr-g5x/q.so
0x74135e0000-0x74135e7000 r--p   .../powervr-g5x/libvulkan_powervr.so   <- the ICD
0x74135e7000-0x74135f6000 ---p   .../powervr-g5x/libvulkan_powervr.so
```

**`0x74135b0000` - the base `dladdr` reports and the hook addresses are built from - is not mapped at
all.** It sits in the gap between `q.so` and the ICD. libhybris is at `0x740d2d0000`. The difference is
`0x62e0000`.

So the defect, as tightly as the measurements allow: **the base used to form the hook addresses for the
HAL's interposed imports is wrong, and `dladdr` agrees with the wrong base** - which points at the
loader's view of where libhybris is, not at the hook table's contents (`name`, `func` and `debug_func`
are all the same, consistent value).

**The remaining question is now specific and narrow:** why does the loader's base for libhybris differ
from its actual mapping, by `0x62e0000`. That is inside libhybris's own loader interaction
(`hooks.c`'s `__hybris_load_linker` / `dlopen` path and what it does with the returned handle) and is
where the fix belongs. It is not the ICD, not `q.so`'s own linker, and not the app.

**Instruments:** `hookret_log` in `hybris/common/hooks.c` (filtered to `vulkan.powervr`, own file,
no DEBUG build), plus `docs/development/pvr-re/rig/`. `build_hybris.sh` rebuilds both DSOs in one
command, so a candidate fix is one build and one session.
