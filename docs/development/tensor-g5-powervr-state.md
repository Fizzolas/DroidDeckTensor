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

## 32. The returned address is a thunk in a region that is not mapped (2026-10-11)

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

## 34. The base mismatch, measured exactly (2026-10-11)

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

## 35. Clean same-process measurement, and what it rules out (2026-10-11)

The previous sections' base claims were made across processes. This one is not: the hook log and the
crash are the **same pid**, and libhybris's base is taken from that pid's own maps.

```
pid 17327   strcmp /vendor/lib64/hw/vulkan.powervr.so -> 0x704c36bfd8 (hook)
pid 17327   caught signal 11  si_addr=0x704c36bfd8  pc=0x704c36bfd8
pid 17327   libhybris mapped at 0x7046050000   (its own /proc/self/maps)
            so &_hybris_hook_strcmp would be 0x704608bfd8
            returned - real = 0x62e0000, and returned = ICD_base + 0xbfd8
```

But another process got it **right**:

```
pid 16778   returned             0x764f5ebfd8
            REAL &_hybris_hook_strcmp  0x764f5ebfd8     <- identical
            (libhybris base 0x764f5b0000, offset 0x3bfd8)
```

**So the returned address is correct in some processes and wrong by `0x62e0000` in others.** That is
what every fixed hypothesis in §31-34 failed to survive: the defect is not a constant wrong base and
not a wrong table entry - both would be the same in every process. It is **process-dependent**.

**What this rules out**, cleanly:

* not "the table entry is wrong" - in the process where it was checked, `name`, `func` and
  `debug_func` are consistent and `func` equals the real function address;
* not "a constant wrong base" - one process has no error at all;
* not our ICD, the HAL, Steam, or `q.so`'s own linker - the value comes out of libhybris's resolver.

**What it points at:** something per-process about how the hook address is formed or relocated - the
order in which libhybris, `q.so` and the ICD are loaded, or a relocation applied against the wrong
module's base in some orders. The next measurement should log, **in the faulting process**, the
relocation of the hook table entry (what writes `func`) rather than the value read out of it.

**Process note, for whoever continues this.** Four successive root-cause claims in §31-34 were wrong and
each was killed by the next measurement. The cause was inference outrunning measurement, and twice it
was comparing addresses across processes. The instruments are sound (`hookret_log`, the rig, same-pid
comparison); the discipline of using them on one process at a time is what has to hold.

## 36. Why §31-35 could not converge: the measured build was never the shipped build (2026-10-11)

§35 is right that the defect is process-dependent. But the four root causes before it were not only
"inference outrunning measurement" - they were **measurements of different binaries**, and nothing in
the tree could tell. This section is the outer-loop fix, and it is the prerequisite for any further
libhybris work: without it, the next fix is unverifiable too.

**The build was moving under the measurements.** `_hybris_hook_strcmp`'s offset in the built
`libhybris.so`, across the sections that were all narrated as one continuous thread:

```
§33 0x3bf94      §34 0x3c080      §35 0x3bfd8      (dist/, shipped = 0x3bd9c)
```

Four builds, four numbers, one story. §33's "off by 0xa3a0000" and §35's "off by 0x62e0000" are the
same symptom measured against different binaries, so the numbers never reconciled.

**Why: the build output and the shipped artifact were two different directories.** Every
`build_hybris.sh` run copied to a scratch drop (`/mnt/d/GUH/`); `package_asset.py` ships
`docs/development/pvr-re/hybshim/dist/`, which changed only by hand. Measured:

```
shipped  dist/libhybris.so    sha256 d7508713...   (mtime 2026-10-10 22:02)
under test /mnt/d/GUH/...so   sha256 c9b54d74...   (mtime 2026-10-11 08:39)
```

And `build_icd.sh` - which `package_asset.py` claims to read - **existed only loose at
`/mnt/d/GUH/build_icd.sh`**, never committed, while the README's ICD one-liner omitted `pvr_wsi.c`.
The guest's installed `libhybris.so` was `c9b54d74` because it had been **hand-pushed** to match the
measurement; the next clean apk install would have shipped `d7508713` and silently regressed.

**The toolchain is fine; the coupling was not.** Rebuilt twice, the shim and ICD are byte-identical.
So the fix is not in the compiler but in what feeds it and where it lands:

* `build_hybris.sh` writes into `dist/`; `build_icd.sh` is committed and writes into `icd/`.
* `build_all.sh` runs both and then `build_id.sh`, which stamps `dist/build_id.json` with the sha256
  of each artifact and the libhybris commit/branch they came from.
* `package_asset.py` **refuses to package** unless `dist/` matches that stamp, and the asset carries
  `build_id.json` so a running guest's driver can be checked against the build it claims to be.
* The instrumentation that produced §31-35 (`hookret_log` in `hooks.c`) is committed on
  `droiddeck-pvr-rig` (`ba22250`) - it was an uncommitted working-copy edit, one `git checkout` from
  being deleted.

**Verified on the device (2026-10-11).** Built, stamped, packaged, installed (versionCode 15, so
`installBundled` re-extracted - the §15 trap), and read back the guest's own files:

```
guest files/linux_vulkan_drivers/powervr-g5x/
  libhybris.so         c9b54d74...   = build_id.json   ✓
  q.so                 ba9b7904...   = build_id.json   ✓
  libvulkan_powervr.so b31d4d2d...   = build_id.json   ✓   source ba22250
```

**The crash baseline reproduces on the shipped build** (session `2026-10-11-11-steam`, the first
run on a driver whose hashes are known): `Initted Wayland backend` -> the update UI's swapchain
(`xid 0x800002`, 2404x1080) -> the client's own swapchain (`xid 0x1200002`, **1x1**, destroyed
immediately) -> `Error: XRRGetOutputInfo() is not available` -> `steam exited rc=139`. Identical to
sessions 44/45, so the fault is stable across builds and the earlier sessions were not lying - they
were just unlabelled.

**What this does not do.** The crash is *unchanged*, not fixed: the guest already ran `c9b54d74`,
and that is what now ships. §35's process-dependence and its next step (log the relocation that
writes the hook table's `func`) stand exactly as written - but they are now answerable, because
"which build was that?" has an answer. The count of sessions that produced nothing measurable
(29->45) is the cost this section is meant to stop paying.

## 37. ROOT CAUSE AND FIX: libhybris.so is unloaded and reloaded, orphaning the HAL's PLT
## (2026-10-11)

§31-35 were all reading the same symptom from one process and calling it a wrong base. The build
identity fix (§36) made the instrument trustworthy, and one same-pid measurement then settled it.
**The address is not wrong - it is right, and then the library it points into is unmapped.**

**The measurement.** Instrumenting `hookret_log` to also dump `/proc/self/maps` at the first
resolution, in the faulting process (pid 23265), and comparing with the crash report's maps, same
pid:

```
AT HOOK RESOLUTION   libhybris.so @ 7628820000   icd @ 76288b0000
                     &_hybris_hook_strcmp = 762885c1b4  (= libhybris + 0x3c1b4, in its r-xp)
AT CRASH             libhybris.so @ 7622540000   icd @ 7628850000
                     si_addr = 762885c1b4  (= libhybris' + 0x63c1b4, NOT MAPPED)
```

`7622540000` (the crash-time base) appears **0 times** in the resolution log; `7628820000` (the
resolution-time base) appears **0 times** in the crash maps. **`libhybris.so` is unloaded and
re-loaded at a different base during the process's life.** The HAL holds the old copy's function
addresses - resolved by libhybris's own linker, which is why glibc's `dlopen`/`dlclose` accounting
cannot see that the HAL depends on libhybris - and the first call through one faults.

That is what §35's "process-dependent" really was: whether the process happened to keep libhybris
at the base it resolved against. And it explains every earlier reading: the "anonymous PROT_NONE
region" (§26) was the old copy's r-xp segment, correct at resolution and unmapped at the crash.

**The fix: `-Wl,-z,nodelete` on `libhybris.so`.** `build_hybris.sh` now links it with
`DF_1_NODELETE`, so glibc can never unmap it while the process lives. This is principled, not a
workaround: the HAL holds raw pointers into libhybris's code, so libhybris must outlive every user -
exactly what `nodelete` is for.

**Measured effect.** The libhybris fault is **gone**: the crash is now `si_addr=0x0`, `pc=0x0`, and
libhybris has one mapping, not two. The process gets materially further and dies later. So this fix
is real and it moved the wall - it is not a complete fix, and §38 is the next fault it exposed.

## 38. The fault after the remap fix: a NULL call in gamescope's FROG WSI layer (2026-10-11)

With libhybris pinned (session `2026-10-11-13-steam`), `steam exited rc=139` still - but a
**different** fault, previously masked:

```
comm=steam  caught signal 11  si_addr=0x0  pc=0x0
lr = 73655d4834 = libVkLayer_FROG_gamescope_wsi_aarch64.so + 0x14834
```

Disassembled at that offset (`BLR x3` with `x3 = [x20+0x38] = 0`):

```
14820: LDR x3,[x20,#0x38]     ; a dispatch/function pointer
1482c: MOV x0,x19
14830: BLR x3                 ; -> NULL
```

So the FROG layer calls a NULL entry out of a function table. It is reached right after the client
creates its swapchain (`xid 0x1200002`) and the log shows **`Destroying swapchain` twice** for the
same handle - the client is tearing its surface down, then calls into a table entry the layer never
populated. It is on the client's X11/present teardown path, and it is *not* the ICD (the ICD's own
`pvr_wsi` calls all returned). Next: what that table is and which entry is NULL - the FROG layer is
the gamescope-side Vulkan WSI, so the entry is one of the Vulkan functions the client expects the
layer to chain.

**The two fixes so far, both measured:** §36 (build identity - so a measurement means something) and
§37 (`nodelete` - so the HAL's PLT stops pointing at unmapped code). Neither is the last wall, but
each moved the crash to a strictly later point, and for the first time the movements are traceable.

## 39. The FROG NULL call is `vkAcquireNextImage2KHR`, which the ICD did not provide - and the
## segfault is now gone (2026-10-11)

§38's `BLR x3` with `x3 = 0`: `x20` is gamescope's `vkroots::VkDeviceDispatch` and `[x20+0x38]` is
member 7 of it. The layer's own `AcquireNextImageKHR` (source read, not guessed) is:

```cpp
static VkResult AcquireNextImageKHR(pDispatch, ...) {
  VkAcquireNextImageInfoKHR acquireInfo = { ... };
  return AcquireNextImage2KHR(pDispatch, device, &acquireInfo, pImageIndex);  // -> pDispatch.m_AcquireNextImage2KHR
}
```

and **our ICD returned NULL for `vkAcquireNextImage2KHR`** (measured: every session's
`ICD gdpa ... name=vkAcquireNextImage2KHR -> (nil)`), because `pvr_wsi_proc` served only the 1KHR
form. So the loader left that dispatch slot NULL and the layer called through it.

**Fix:** `pvr_wsi.c` now serves `vkAcquireNextImage2KHR` (forwarding to the same logic as the 1KHR
form - the only difference is the info struct).

**Measured effect: the segfault is gone.** Session `2026-10-11-18-steam` has **no `rc=139`** and
`crash.txt` was not rewritten (still the previous session's), so the crashcatch never fired. Steam's
client gets past the point it has died at in every session since 43.

**The next wall, also measured, and it is a different kind:** the client's frames now reach
gamescope and gamescope **cannot import them** -

```
[gamescope] wlserver: [types/wlr_linux_dmabuf_v1.c:219] Failed to import DMA-BUF FD for plane 0: Invalid argument
[destroyed object]: error 7: importing the supplied dmabufs failed
MESA: error: zink: failed to update swapchain capabilities: VK_ERROR_SURFACE_LOST_KHR
```

Our WSI exports a dma-buf per swapchain image (`pvr_wsi: swapchain: 3 images 2404x1080 stride 9616
fmt 44`) and gamescope's linux-dmabuf importer rejects the fd. That is a **format/modifier or plane
descriptor** mismatch between what we advertise on the exported image and what gamescope's
`zwp_linux_dmabuf` import expects - the next thing to fix, and a normal WSI-integration problem
rather than a crash.

**Where this stands:** four measured steps, each moving the wall strictly later - §36 build identity,
§37 `nodelete`, §38-39 the `AcquireNextImage2KHR` slot. The client no longer crashes; it renders and
presents, and the compositor refuses the buffer. The crash that cost sessions 29-45 is closed.

## 40. The dma-buf import wall: the HAL faults on DRM-format-modifier images (2026-10-11)

With the crash closed (§37-39), the client's frames reach gamescope and gamescope cannot import
them: `wlr_linux_dmabuf_v1.c:219 Failed to import DMA-BUF FD for plane 0: Invalid argument`.

**Reproduced in isolation** (`probes/vk_dmabuf_import_test.c`, exported BGRA8 LINEAR exactly as
`pvr_wsi.c` does, then imported the way a compositor does):

```
vkGetMemoryFdPropertiesKHR -> 0  memoryTypeBits=0x6
import fd as a plain LINEAR image        -> vkAllocateMemory=0
import the SAME fd as a DRM_FORMAT_MODIFIER image -> vkCreateImage=0,
    vkGetImageMemoryRequirements -> size=16384 typeBits=0x7,
    vkAllocateMemory -> SEGFAULT
```

So the fd is valid and importable; **the HAL segfaults when the importing image uses
`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`** - the tiling a compositor's Vulkan renderer uses. The
HAL's `VK_EXT_image_drm_format_modifier` support is broken for this (its `vkGetImageDrmFormatModifier
PropertiesEXT` and modifier import path are not sound).

**First mitigation, shipped and measured.** The ICD now **suppresses
`VK_EXT_image_drm_format_modifier`** from device-extension enumeration, so a compositor does not take
the modifier path:

* Before: gamescope logged `physical device supports DRM format modifiers`, imported with modifiers,
  and the client died (`guest.exited status 1`).
* After (session `2026-10-11-22-steam`): gamescope logs `physical device does not support DRM format
  modifiers`, and the session **exits 0 with no crash**, running much longer.

**Not yet solved - the import still fails, and the reason is not the modifier path alone.** After the
suppression gamescope still logs two `Failed to import DMA-BUF FD ... Invalid argument`, for the
buffers at real sizes (2404x1080 for the update UI's `xid 0x800002`, 64x24 for a client surface
`0xe00005`). The 1x1 client swapchains (`0x1200002`, `0x1400002`) are a *consequence*: our WSI's
`wsi_surface_caps` reads the window geometry, gets nothing for a not-yet-mapped X window, and reports
`currentExtent = UINT32_MAX`; the client then picks 1x1.

**So the remaining question is why wlroots' linux-dmabuf import rejects our fd** even without the
modifier path. Our glibc probe imports the same fd successfully, so the difference is in how the
*compositor* imports (EGL/GBM `EGL_EXT_image_dma_buf_import` vs Vulkan external memory, or a
format/plane check wlroots does before the driver call). The next step is to read gamescope's
`wlr_linux_dmabuf_v1.c:219` (its wlroots fork, not upstream) and match the check it fails.

**Also fixed in this step:** `icd_enum_device_ext` returned `VK_INCOMPLETE` whenever any extension was
filtered, which made the loader treat the enumeration as truncated and the app see
`VK_KHR_external_memory_fd` et al. as *missing* (gamescope logged exactly that). `VK_INCOMPLETE` now
means only "more than fit", per spec.

**Net progress this session:** build identity (§36), the crash (§37-39, gone), and the modifier-path
mitigation. The client runs to exit 0 instead of crashing; frames still do not reach the screen
because the compositor will not import the buffer.

## 41. THE IMPORT WALL WAS A STALE SHIM, not the driver (2026-10-11)

§40 said the import failed for the real-size buffers even after suppressing the modifier path. The
cause was not the driver, the format, or the modifier - **the deployed `libblsession.so` was built
before the GEM fix (`93212b4`) and had never been rebuilt.** This is the same "measured source is not
the shipped binary" class as §15 and §36, now in the *shim*.

**The exact check, read from wlroots' source** (`types/wlr_linux_dmabuf_v1.c`, the `:219` in the
error):

```c
static bool check_import_dmabuf(struct wlr_dmabuf_attributes *attribs, void *data) {
    ...
    if (drmPrimeFDToHandle(linux_dmabuf->main_device_fd, attribs->fd[i], &handle) != 0) {
        wlr_log_errno(WLR_ERROR, "Failed to import DMA-BUF FD for plane %d", i);
```

gamescope validates every incoming buffer by calling **`drmPrimeFDToHandle`** on its main DRM device -
a **GEM** ioctl. The PowerVR render node is `pvrsrvkm` and has no GEM, so it answers EINVAL, and
gamescope refuses the buffer. `tools/linuxfs/preload/drm.c` exists precisely to stand in for GEM on
such a node; its predicate was fixed for PowerVR in `93212b4`.

**Measured, before and after rebuilding the shim from the current source:**

```
session 22 (old libblsession.so):  zink: failed drmPrimeFDToHandle  x many
                                   Failed to import DMA-BUF FD for plane 0  x2
session 23 (rebuilt shim):         zink: failed drmPrimeFDToHandle  0
                                   Failed to import DMA-BUF FD for plane 0  0
                                   steam exited rc=0 (clean, ~43 s)
```

**So the frame path is now unblocked at the compositor.** gamescope imports our dma-buf; the client
no longer dies on it.

**Two things this implies, both worth acting on:**

1. **The app's `linuxfs` asset (which carries `libblsession.so`) was stale.** It is gitignored and
   built by CI / `tools/build_local.sh` from `tools/linuxfs/preload/*.c`, so a fresh build picks up
   the fix - but any device running an older install keeps the old shim, and the symptom looks
   exactly like a driver bug. **A shim fix that is not rebuilt into the asset is not shipped.**
2. The remaining failures are different and smaller: `Error getting buffer`, an X `BadAlloc` on
   DRI3 (major opcode 147), and the client's swapchains still being **1x1** - see §42.

**Reproduce the shim rebuild (no Docker):**
```
zig cc -target aarch64-linux-gnu.2.38 -shared -fPIC -O2 -pthread \
    -o libblsession.so tools/linuxfs/preload/*.c -ldl
```

## 42. gamescope imports our dma-buf now: the HAL only binds imported memory LINEAR (2026-10-11)

§40 left two failures: gamescope's renderer `vkBindImageMemory failed (VkResult: -1)` many times, and
the client's X `BadAlloc` on DRI3. The first is fixed; the second is the current wall.

**Root cause, isolated** (`probes/vk_dmabuf_import_test.c`, step 2b): importing a dma-buf into an
**OPTIMAL-tiling** image faults inside the HAL's `vkAllocateMemory`/`vkBindImageMemory`, exactly as a
`DRM_FORMAT_MODIFIER` image does (§40). The same fd binds fine to a **LINEAR** image. The HAL only
supports LINEAR for imported dma-buf memory.

**Why gamescope hit it.** A compositor imports a client's buffer with createFlags
`{bSampled, bTransferSrc}` and neither `bMappable` nor `bLinear`, so its tiling is **OPTIMAL**
(`rendervulkan.cpp:2102`: `tiling = (bMappable || bLinear) ? LINEAR : OPTIMAL`). With
`VK_EXT_image_drm_format_modifier` suppressed (§40) it cannot take the modifier path either, so it
lands on OPTIMAL - the one tiling the HAL cannot import into.

**Fix, in the ICD:** `icd_create_image` rewrites an **external** image's tiling to LINEAR before the
HAL sees it. An imported image's tiling only affects how the GPU addresses the buffer; the fd is a
LINEAR layout either way. Non-external images are untouched. The wrapper is registered in both
`vk_icdGetInstanceProcAddr` and `icd_gdpa` (a core function like `vkCreateImage` is fetched through
GIPA to fill the loader's device dispatch, so GIPA is the one that matters).

**Measured, session `2026-10-11-25-steam`:**

```
vkBindImageMemory failed   many -> 0
Failed to import DMA-BUF     0
rc=139                       0
steam exited rc=0, ~43 s
```

**Remaining wall: `Error getting buffer` and X `BadAlloc` (major opcode 147, minor 7 =
DRI3 PixmapFromBuffers).** That is the *client -> Xwayland* present path (our `pvr_wsi.c` X11 branch
calls `xcb_dri3_pixmap_from_buffers`), not gamescope's Vulkan import. Xwayland's DRI3 rejects the
pixmap; the next step is to see which fd/node Xwayland's DRI3 uses and whether the GEM shim covers
that path (it is a different call site from wlroots' `drmPrimeFDToHandle`). The client's 1x1
swapchains remain and are likely the same story: a window whose size the WSI cannot read yet.

**Caveat to keep in mind:** forcing LINEAR for external images changes how gamescope addresses any
external image it creates, including its own output. The session runs clean (`rc=0`), but if output
images look wrong later, this is the place to look.

## 43. Where the frame stands: compositor import works, the last walls are X11/DRI3 and Wayland size
## (2026-10-11)

After §42 (gamescope binds our dma-buf), sessions `2026-10-11-25..27-steam` are stable: `rc=0`, no
crash, no import error, no `vkBindImageMemory failed`, ~43 s. Two walls remain before a frame is on
screen, both now measured rather than guessed.

**1. The client's swapchains are 1x1, and they are Wayland surfaces.** The caps diagnostic
(`pvr_wsi: caps: window 0x0 kind=0 unreadable`) shows them as `kind=0` (Wayland) with no window. A
`wl_surface` genuinely has no size before the first buffer is committed, so `wsi_surface_caps`
reports `UINT32_MAX` ("app chooses") and the client picks 1x1. To give it a real size the WSI must
know the compositor's configure - either track the surface's role (`xdg_surface.configure`, which
needs the xdg-shell globals) or use a plausible output size. `xdg-shell` tracking is the correct fix.

**2. X `BadAlloc` (major opcode 147, minor 7 = DRI3 `PixmapFromBuffers`).** Our X11 present path
*does* run and succeed - `pvr_wsi: X11 present: window=0x2000034 32x32 stride=128 depth=24 bpp=32`
for the 32x32 windows - so the BadAlloc is a *different* DRI3 request, most likely the client/CEF's
own X11 present (CEF is launched `-cef-ozone-platform=x11`). `Error getting buffer` (Steam) follows
it. The next step is to identify the requesting process and whether Xwayland's DRI3 rejects it for
the same no-GEM reason the wlroots check did (§41) - if so, the same shim covers it; if not, it is a
pixmap-format/param mismatch in our `xcb_dri3_pixmap_from_buffers` call.

**Net state of the guest:** load -> ICD -> device -> render -> export dma-buf -> **gamescope imports
and binds it** -> Xwayland/X11 present runs. The compositor can now consume our frames; what is left
is the client's own X11 present and the surface-size question, not the GPU path.

## 44. The Wayland surfaces now report a real size - the client still chooses 1x1 (2026-10-11)

The WSI now tracks the compositor's `wl_output` mode (`reg_global` binds `wl_output`; `out_mode`
records the current mode) and reports it as the surface size when a Wayland surface has none, so a
fullscreen surface gets a real `currentExtent` instead of `UINT32_MAX`. Measured (session
`2026-10-11-30-steam`, the capped caps diagnostic):

```
caps: kind=0 window=0x0 geom=2404x1080 out=2404x1080   (x25)   <- Wayland, real size now
caps: kind=1 window=0x1c0002f geom=32x32 out=-1x-1      (x6)    <- X11 32x32 windows
```

**And the client still creates 1x1 swapchains.** So the 1x1 is the *client's* choice, not our
`currentExtent`: gamescope hands the game a surface whose viewport is 1x1, and the game sizes its
swapchain to that. The size a client actually gets is negotiated through xdg-shell
(`xdg_toplevel.configure`); the WSI does not carry xdg-shell, and the 1x1 configure comes from
gamescope. So this is a gamescope-side sizing question (its nested output geometry), not the ICD -
and it is separate from the still-present X `BadAlloc` on DRI3, which is the client/CEF's own X11
present (our WSI's X11 present runs only for the 32x32 clip windows).

**Net state:** the compositor imports and binds our frames (§42); our X11 present runs; the Wayland
path has an honest surface size. The two remaining items are both about *what size and how the
client presents* - gamescope's surface geometry, and the client's X11/CEF DRI3 - not the GPU path,
which is proven end to end.

## 45. The wall is CEF's GPU process: it uses GBM, and the PowerVR node has no GBM (2026-10-11)

Sessions 34 and 35 were read from their artifacts (the device was reconnected and a fresh session
run to confirm). Both are `rc=0`, no crash, ~43 s, and both end `WaitingForCredentials` in
`steam/steamui_login.txt`. Two things in them were previously read wrong and are corrected here.

**The update UI presented 21 full-res frames; the main UI never rendered.** §43-44 read the 21
`pvr_wsi: Wayland present: 2404x1080` lines as the client presenting. They are the **update UI**
(Steam's `-gamepadui` verification window `0x800002`, drawn via **glx** - `session.log` line 279,
`Using update UI: glx`). It runs `Show window` -> 21 presents -> `Destroy window`. The **main
GamepadUI** then opens `0x1200002` (`steamui_system.txt`: `Current user is not logged in` ->
`Initializing BUI`), makes a 1x1 swapchain, and never presents. `droiddeckctl state` agrees:
`firstFrame: false`.

**Root cause: CEF's GPU process cannot allocate a buffer and segfaults.** The Steam UI is CEF
(Chromium 126). From `steam/steamwebhelper.log` and `steam/cef_log.txt` (identical in 34 and 35):

```
steamwebhelper.log: ERROR:gbm_wrapper.cc(74)]  Failed to get fd for plane.: No such file or directory (2)   x40
steamwebhelper.log: ERROR:gbm_wrapper.cc(257)] Failed to export buffer to dma_buf: No such file or directory (2)
cef_log.txt: ERROR:shared_image_interface_proxy.cc(130)] Buffer handle is null. Not creating a mailbox from it.
cef_log.txt: ERROR:one_copy_raster_buffer_provider.cc(348)] Creation of StagingBuffer's SharedImage failed.
cef_log.txt: ERROR:gpu_process_host.cc(1002)] GPU process exited unexpectedly: exit_code=11
cef_log.txt: WARNING:gpu_process_host.cc(1436)] The GPU process has crashed 1 time(s)
```

Measured per session: **13-14 `GPU process exited unexpectedly` and 40 `Failed to export buffer to
dma_buf`.** CEF's GPU process drives its own buffers through **GBM** (`gbm_wrapper.cc`), and the
PowerVR render node has **no GBM and no GEM** (§41) - `gbm_bo_create`/export fail, the resulting
shared-image handle is null, and the GPU process takes SIGSEGV (`exit_code=11`). It is relaunched
(from `start count: 0` to `1`) and crashes again. So the UI's renderer never comes up, which is why
`GAMESCOPE_VIEWPORT_SUPPORTED` is unreadable and the window is 1x1.

**This is a new wall, distinct from the DRI3 `BadAlloc`.** The `BadAlloc` is on 32x32 X11 windows
(`0x2000034`, `0x1c00034`) - CEF helper windows - and is the *same* no-GEM failure seen one layer
down. The `tools/linuxfs/preload/drm.c` shim stands in for GEM, but **only for the libdrm API**
(`drmPrimeFDToHandle`, `drmPrimeHandleToFD`, `drmCloseBufferHandle`) and the `GEM_CLOSE` ioctl
(`netif.c`). It does **not** interpose:
- `PRIME_FD_TO_HANDLE` (`DRM_IOCTL_PRIME_FD_TO_HANDLE`, `_IOWR('d',0x2e,...)`) - so a caller that
  issues the ioctl itself (CEF via libgbm/Mesa, rather than libdrm's wrapper) gets no handle; and
- the **GBM** API (`gbm_create_device`, `gbm_bo_create`, `gbm_bo_get_fd`, ...) at all.

**Where this points next (not yet done):**
1. Interpose `PRIME_FD_TO_HANDLE` in the ioctl path the same way `GEM_CLOSE` is (a table of
   duplicated dma-buf fds keyed by fd+handle), so a direct-ioctl caller is covered too.
2. Interpose the GBM API (`libgbm.so.1`) so `gbm_bo_get_fd`/plane-fd calls return the dma-buf the
   caller already has, instead of going to the (absent) kernel GBM. This is the layer CEF's
   `gbm_wrapper.cc` actually calls.
3. Cheaper experiment first, to bound the work: force CEF's GPU process off the GBM path and onto
   SwiftShader/software GL. The session already launches the client with `-cef-use-gl=angle
   -cef-use-angle=vulkan`; an A/B with a different `-cef-use-angle` backend (or CEF's
   software/disable-gpu switches) tells us whether a frame reaches the screen with the GPU path
   bypassed, i.e. whether GBM is the *only* thing between here and a visible login screen.

**The build-identity caveat, live again (§36).** The device's installed driver is
`libhybris.so 037c8e87 / libvulkan_powervr.so b31d4d2d / q.so ba9b7904` (its `build_id.json`,
`builtAt 2026-10-11T13:15:54Z`), while the committed `hybshim/dist/build_id.json` is
`libhybris.so 058bdaed / libvulkan_powervr.so 68f90ad8 / q.so ba9b7904` (`builtAt
2026-10-11T20:35:04Z`). They differ, so **sessions 34-35 did not run the committed dist**. The ICD
the device runs *does* carry the e60ba95 diagnostics (verified by `strings`), so the era is right,
but before treating any of the above as "the current build", rebuild with `build_all.sh` +
`package_asset.py`, bump `versionCode`, and re-hash the guest.

## 46. CEF's GPU process crashes on every backend; the main UI window stays 1x1 (2026-10-11)

§45 named CEF's GPU process (GBM, `gbm_wrapper.cc`) as the wall. This section is the A/B that tried
to move it, and what the results say. **Nothing here got a frame; the value is the negative result
and the method.**

**The experiments** (all via `Download/droiddeck-env`, read by the session script's `BL_CEF_*`
switches, added this session; each session otherwise identical: `rc=0`/idle, ~43 s, no crash of the
client itself):

| session | CEF config | update-UI presents | main-window 1x1 | CEF `GPU process exited` |
|---|---|---|---|---|
| 34/35/41 | default (`-cef-force-gpu -cef-use-angle=vulkan`) | 21 | yes | 13-28 |
| 36 | `-cef-disable-gpu` | 21 | yes | 0 in that log (still 1x1) |
| 37 | `-cef-use-angle=swiftshader` | 41 | yes | 20 |
| 38 | `-cef-ozone-platform=wayland` | 21 | yes | 21 |
| 40 | `-cef-use-angle=gl` | 21 | yes | 28 |
| 44 | `VK_ICD_FILENAMES=.../vk_swiftshader_icd.json` (client only) | 21 | yes | 34 |
| 46 | gamescope `--force-windows-fullscreen` | 21 | yes | 35 |
| 50 | `-cef-use-gl=desktop` (GLX) | 21 | yes | 49 |

**Two facts fall out, and they matter more than any single run:**

1. **The main GamepadUI window (`0x1200002`) is 1x1 in every configuration.** No CEF or gamescope
   flag moved it. So the 1x1 is not a CEF-GPU artefact and not the gamescope surface size - it is
   what the client asks for, from a size it decides before it renders. The update UI's window
   (`0x800002`) is the only one that ever gets a real size (2404x1080) and presents.
2. **CEF's GPU process crashes on every backend** - Vulkan, ANGLE-GL, SwiftShader-ANGLE, GLX, and
   (with `-cef-disable-gpu`) it still reports crashes. A crash common to *all* of them is not a
   backend/GBM problem; it is something the GPU process does before or regardless of the backend
   (its own startup: libpci/udev/PCI enumeration, shared memory, or its sandbox). The
   `gbm_wrapper.cc`/`Buffer handle is null` lines are real but are one symptom, not the whole fault.

**Method note, learned here.** `Download/droiddeck-env` is read with a **one-session lag**: a
session that starts within a few seconds of the file being rewritten reads the *previous* contents.
Sessions 48 and 49 proved it (48 ran the config written for it but logged session 47's). Always start
the session at least ~5 s after writing the env, and **confirm the switch line in the new
`session.log`** (`== steam: CEF ozone=... use-gl=...`) before trusting a result.

**Where this points.** The GPU process's own output is the missing evidence, and it is not in any
Steam log (crashpad swallows the abort; `--enable-logging=stderr` does not reach the GPU process's
argv). The rig's catcher was extended to gate on `comm=steamwebhelper` and delivered via the app's
`LD_PRELOAD` (not `/tmp`, whose presence would trip the script's `CRASHCATCH_ALL=1`), but it produced
no report - the GPU process dies in a way the in-process handler does not see. The next step is a
real debugger in the guest (none is installed) or capturing the GPU process's stderr by wrapping its
launch. Until the GPU process's fault is known, the CEF flags are exhausted.

**Also fixed in passing:** the debug rig's `crashcatch` was preloaded into every session process with
`CRASHCATCH_ALL=1` (its `.so`s were left in `files/linuxfs/tmp/`), which the rig's own README warns
turns recovered faults into kills - one `crash.txt` read `comm=Xwayland caught signal 11`. Removed
for the clean baseline (session 41); it did not change the 1x1 or the CEF crash, so it was not the
explanation, but it should not be left in the guest.

**Addendum to §46 (same session).** `--in-process-gpu` (session 52) put the GPU logic in the client
process and made a second fault visible, but it is a **teardown** crash, not the UI blocker:

```
== steam exited rc=0
(EE) Backtrace: ... libgallium-26.2.3-arch1.1.so ... ld-linux (_dl_deallocate_tls+0x28)
(EE) Segmentation fault at address 0xfffffffffffffff0
Fatal server error: Caught signal 11. Server aborting
```

Xwayland crashes in Mesa's **libgallium** while tearing down (the `comm=Xwayland` crash.txt from
earlier is the same one). It happens *after* the client exits, so it is not why the UI is 1x1 - but
it is a real second bug, and on a device where Xwayland dying takes the session with it, it is worth
its own look. The main window was still 1x1 with `--in-process-gpu`.

**Addendum 2 to §46 - the guest has no DRM *card* node (measured).** `LinuxRuntime.bindGpuNode`
builds the session's `/dev/dri` stand-in and adds a card node from `cardNodeFor()`, which reads
`/sys/dev/char/226:128/device/drm/card*/dev`. On this device that read is **denied** (`app.log`:
`drm stand-in incomplete: AccessDeniedException .../sys/226:128/device/drm/card2/dev`), so
`cardNodeFor` returns null, the `if (card != null)` block is skipped, and the guest gets:

```
cache/drm/dri/renderD128      <- the only node
cache/drm/sys/226:128/device  -> symlink to the real sysfs (readable)
cache/drm/sys/240:16          <- some other node
```

i.e. **`renderD128` and no `card*`.** Everything built on libdrm's primary-node notion is affected:
gamescope's DRM backend (§ gamescope-sdl-zero-ext, worked around in the ICD) and, relevant here,
**GBM** - `gbm_create_device()` is given a card node by its callers, and Chromium's
`gbm_wrapper.cc` (the source of §45's `Failed to get fd for plane`) is one of them. A guest with no
card node is a plausible reason CEF's GBM buffer path cannot work even before GBM's absent kernel
backend is considered.

The fix is in the app, not the driver: when `cardNodeFor` cannot read the real sysfs, describe the
card node from the numbers the device probe already knows (`primary=226:2`, `render=226:128` on this
Tensor G5) rather than creating none - the same information `cardNodeFor` was trying to read.
`GpuInfo`/the probe already carry the render node's device numbers, and the ICD already reports
`hasPrimary=1 primary=226:2`, so the card minor is known without sysfs.

**But this is not the whole story:** CEF's GPU process also aborts on the backends that never touch a
card node (GLX/`-cef-use-gl=desktop`, ANGLE-GL), so the card node is a necessary piece at most, not
sufficient. It is nonetheless the most concrete, fixable gap found this session.

## 47. The guest had no DRM card node: a read-only-sysfs write aborted the stand-in (2026-10-11)

`LinuxRuntime.bindGpuNode` describes the GPU's card node so the session's `/dev/dri` has one - needed
by gamescope's Wayland backend (a primary node) and, more recently relevant, by GBM, whose callers
(Chromium's `gbm_wrapper.cc`) open a **card** node. It was silently building none.

**The bug.** The card block writes `card*/dev` and `card*/uevent` into `drm.getParentFile()` =
`device/drm/`. When the render node's device directory is the kernel's own, `realDeviceDir()` returns
it and `device` is made a **symlink to the read-only real sysfs** - so those writes land in the
kernel's directory and throw `AccessDeniedException`. The whole `try` block then aborts, and the one
line that actually matters for the guest - `new File(dri, "card2").createNewFile()` - never runs:

```
app.log: drm stand-in incomplete: java.nio.file.AccessDeniedException:
         /data/user/0/com.droiddeck.launcher/cache/drm/sys/226:128/device/drm/card2/dev
cache/drm/dri/  ->  renderD128          (no card*)
```

The guest's *sysfs* side of the card node was fine all along (it reads `card2/dev` through the
symlink); only the `/dev/dri/card2` device node was missing. `cardNodeFor` itself was not at fault -
it reads `card2/dev` and gets `226:2`; the writes that followed it were.

**The fix.** The sysfs writes are only needed when the tree is fabricated (`real == null`, an
Adreno); where `real != null` the guest reads `card*/dev` through the symlink and only the device
node is missing. Guarding the two writes on `real == null` lets `createNewFile()` run. Measured
after (commit `7c4a1a0`): `cache/drm/dri/` holds **card2 and renderD128**, no "stand-in incomplete"
line, and the guest can open a card node.

**What it changed, measured (session 57):** CEF's GBM now gets *further* - `gbm_bo_create` succeeds
against the card node and the failure moves to the **export**:

```
gbm_wrapper.cc(74)]  Failed to get fd for plane.              40 -> 80 (per session)
gbm_wrapper.cc(257)] Failed to export buffer to dma_buf.
gpu_process_host.cc(1002)] GPU process exited unexpectedly: exit_code=11   52
```

So the card node was a real prerequisite (GBM could not even open a device before), but the wall is
now precisely **GBM export**: `gbm_bo_get_fd` is a GEM operation (`drmPrimeHandleToFD`) and the
PowerVR node implements no GEM. The `tools/linuxfs/preload/drm.c` shim stands in for the libdrm GEM
API, but only for handles *it* minted (from `drmPrimeFDToHandle`); a handle the node's own allocation
produced is not in its table, so the export still fails. Closing this means either extending the
shim to the GBM API surface (`gbm_bo_get_fd`/`gbm_bo_get_plane_fd`) or giving the node a real GEM -
neither done here.

**Also: the 1x1 is gamescope-consistent, not a bug in the ICD.** gamescope's `steamcompmgr.cpp`
treats a 1x1 window as "pretty useless" (line 4026), skips it for focus ("Exclude windows that are
useless (1x1)", line 5139; "If the window is 1x1 then prefer anything else", line 4153), and sizes a
window to the output only when it is detected fullscreen (`window_is_fullscreen(focusWindow) ||
force_windows_fullscreen`, line 4909). The client's main UI window is 1x1 because the client leaves
it there until its renderer comes up - and its renderer is CEF, which is exactly what is crashing.
So the 1x1 is downstream of the CEF GPU-process crash, not an independent sizing bug; §44's framing
of it as a "gamescope surface geometry" question was wrong.

## 48. A GUEST VULKAN APP'S FRAME IS ON SCREEN (2026-10-11, session `2026-10-11-60-vkcube`)

**The goal is met: a real GPU frame from a guest Vulkan app reaches the panel.** vkcube, run through
the session (`tools/droiddeckctl run /usr/bin/vkcube`), rendered on the PowerVR GPU via our ICD and
its frames were composited to the screen by the DroidDeck compositor. Measured, in one session:

```
session.log:  == droiddeck-session ... mode=run /usr/bin/vkcube
session.log:  Selected GPU 0: PowerVR D-Series DXT-48-1536 MC1, type: IntegratedGpu, apiVersion 1.4.317
session.log:  [Gamescope WSI] Executable name: vkcube
session.log:  pvr_wsi: swapchain: 3 images 2404x1080 stride 16384 fmt 44 (fd0=18)   x2
session.log:  pvr_wsi: Wayland present: 2404x1080 ... (wl_buffer attach+commit)     x20
wayland.log:  vulkan  "gamescope" (gamescope) is presenting GPU frames through Wayland: 2404x1080,
                       format AR24, linear (dma-buf: copied into the screen swapchain)
wayland.log:  stats   last 10 s: 13 frames on screen (1.3 fps) | 14 GPU frames from games | 2 window redraws
```

So the whole chain, end to end, for a real (if simple) Vulkan application:
`vkcube -> glibc loader -> our ICD -> libhybris -> bionic PowerVR HAL -> GPU render -> exportable
dma-buf -> gamescope -> DroidDeck compositor -> on screen`. 20 presents from the app, 13-14 frames
composited to the panel at 2404x1080.

**This is the milestone the whole effort was for**, and it says what was actually blocking Steam: not
the GPU path (this proves it works) but **CEF's GPU process** (sections 45-47), which is a Chromium
component using GBM/GEM that the PowerVR node does not implement. A game, or any app that renders
with Vulkan/GL through the ICD rather than through CEF, gets its frames on screen.

**Caveat.** The compositor log says "copied into the screen swapchain" - the path is a copy, not
zero-copy, at 1.3 fps (vkcube is uncapped-ish and the copy is on the CPU path). It is a real frame,
correctly rendered and presented; it is not yet a performance statement.

**Reproduce:** `tools/droiddeckctl run /usr/bin/vkcube` (the session must be stopped/idle first; the
env file `Download/droiddeck-env` must not carry a stale CEF switch - it is read with a lag).

**Addendum to §48 - the GL path works too (session `2026-10-11-62-glxgears`).** `tools/droiddeckctl
run /usr/bin/glxgears` renders through **mesa zink -> our ICD -> the PowerVR GPU** and presents
through our WSI:

```
session.log:  pApplicationName: glxgears   pEngineName: mesa zink
session.log:  [Gamescope WSI] Executable name: glxgears
session.log:  pvr_wsi: swapchain: 3 images 300x300 stride 2048 fmt 44 (fd0=17)
session.log:  pvr_wsi: Wayland present: 300x300 ... x20
session.log:  1030 frames in 5.0 seconds = 205.923 FPS      (steady ~205 FPS)
```

So both guest rendering paths are proven on screen: **Vulkan** (vkcube, §48) and **GL via zink**
(glxgears) - which is the path Steam's own client uses (`pEngineName: mesa zink` in its logs). The
GL path's "Error getting buffer" line (the same one seen with Steam) is non-fatal here: glxgears ran
at 205 FPS.

**What this says about Steam.** Steam's *client UI* is the only thing still blocked, and it is blocked
in CEF's GPU process (a Chromium component, §45-47), not in the driver or the WSI - both of which now
carry real, correctly-rendered frames from two independent guest stacks.

**Addendum 3 to §46 - four more CEF configs, all the same (2026-10-11).** After the card-node fix
(§47), CEF's crash persists, so more of its launch was varied: `-cef-disable-gpu-sandbox
-cef-disable-seccomp-sandbox`, `--disable-gpu-compositing`, `--single-process --no-zygote`, and
`--in-process-gpu`. Every one: 21 update-UI presents, the main window still 1x1, ~50 GPU-process
crashes, ~80 `gbm_wrapper` export failures. CEF's launch flags are exhausted as a lever; the fault is
in the GPU process's buffer path (GBM export, a GEM operation the node does not implement), not in how
it is started.

**Addendum 4 to §46 - GBM is not the crash; Chromium's shared-image handle is (session
`2026-10-11-65-steam`).** A scoped shim (`/tmp/gbmshim.so` via `Download/droiddeck-env`'s `LD_PRELOAD`,
`gbm_create_device` returning NULL for `comm=steamwebhelper` only) removed CEF's GBM use entirely.
Measured: the `gbm_wrapper.cc` plane/export errors went **80 -> 0** - and the GPU process **still
crashed** (54 times), on the same lines as before:

```
shared_image_interface_proxy.cc(130)]  Buffer handle is null. Not creating a mailbox from it.
one_copy_raster_buffer_provider.cc(348)] Creation of StagingBuffer's SharedImage failed.
gpu_process_host.cc(1002)]              GPU process exited unexpectedly
```

So the fault is **not** GBM export. It is Chromium's **shared-image buffer handle**: the GPU process
asks for a buffer handle (a GEM handle / dma-buf) to back a shared image and gets none, then aborts.
`Buffer handle is null` and `StagingBuffer's SharedImage failed` persist with GBM disabled, which
rules GBM out as the cause and names the handle path instead. On this node that handle needs **GEM**
(`drmPrimeFDToHandle`), which PowerVR's render node does not implement (§41) - and our `drm.c` shim
answers only for handles *it* minted, not for one the driver's own allocation produced.

**Conclusion.** Steam's client UI is blocked by a **Chromium shared-image/buffer-handle requirement
that needs GEM**, a GPU capability this node does not have. The guest's GPU path is otherwise sound -
proven on screen by vkcube (Vulkan) and glxgears (GL/zink, §48). Making CEF render would need a real
GEM/GBM backend on the PowerVR node (or a Chromium build that does not use shared-image handles for
its raster path), not another launch flag or a shim. The CEF launch surface is exhausted.

**Addendum 5 to §46 - it is not the ICD or Vulkan either (session `2026-10-11-72-steam`).** Forcing
CEF fully off Vulkan (`--use-angle=swiftshader --enable-unsafe-swiftshader
--disable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan,AcceleratedVideoDecodeLinuxGL`) still
crashes the GPU process (58x, `Buffer handle is null` 250x). Combined with addendum 4 (GBM removed,
still crashes) and the sandbox/backend matrix, the conclusion is now forced: **CEF's GPU process
cannot run in this guest regardless of backend, sandbox, or GBM.** It is not our ICD (SwiftShader does
not touch it), not Vulkan, not GBM, not the sandbox. What remains is Chromium's platform buffer-handle
path (GpuMemoryBufferManager/shared-image), which needs a Linux DRM/GEM facility this node does not
provide, and the guest's proot environment on top of that.

**This is the end of the line for the flag/shim approach.** Getting Steam's client UI up needs either
(a) a real GEM/GBM backend on the PowerVR node (kernel-level, not ours to add), (b) a Chromium/CEF
build that does not use platform shared-image handles for its raster path, or (c) running CEF's GPU
process outside the environment that denies it those facilities. The guest's *GPU path* is proven
(vkcube, glxgears, §48); Steam's *client UI* specifically needs CEF, and CEF needs facilities this
device's driver does not expose.

**Addendum 6 to §46 - the definitive negative (2026-10-11, sessions 74-80).** After the card-node
fix (§47), the remaining CEF levers were tried and every one failed to change the crash:

- **PRIME ioctls** - CEF issues `DRM_IOCTL_PRIME_FD_TO_HANDLE` (247/session) and
  `PRIME_HANDLE_TO_FD` (240) as **raw ioctls** on the render node; the GEM stand-in covered only the
  libdrm wrappers. `bl_drm_prime_ioctl` now covers both (measured: 258 calls, all `rc=0` with a valid
  handle) - and the GPU process **still** aborts on the same lines. So "Buffer handle is null" does
  not come from that ioctl.
- **Shared-memory backing** (`--disable-gpu-memory-buffer-video-frames
  --disable-gpu-memory-buffer-compositor-resources`) - still crashed (62x, 341 null).
- Earlier: GBM removed (still crashed), Vulkan off / SwiftShader (still crashed), sandbox off (still
  crashed), 20+ launch configs (all identical).

**Conclusion, measured to exhaustion:** CEF's GPU process cannot run in this guest. It is not our ICD
(SwiftShader does not touch it), not Vulkan, not GBM, not the PRIME ioctls, not the sandbox, not any
launch flag. What remains is a Chromium-internal path (its GpuMemoryBuffer/shared-image bookkeeping)
that produces a null handle for reasons not reachable from the driver side or the launch. Getting
Steam's client UI up needs a different Chromium/CEF build or a different environment for its GPU
process - not a change this app or its shims can make.

**The GPU path itself is proven and unaffected:** vkcube (Vulkan) and glxgears (GL/zink) both render
on the PowerVR GPU and present frames on screen (§48), verified again after every one of these
changes. Games and apps that render through the ICD get frames; only Steam's CEF-based UI does not.

**Addendum 7 to §46 - the last levers, and why they are closed (2026-10-11, sessions 81-86).**

- **CEF's GPU process dies by signal, with no CHECK/FATAL message.** A `write()` interposer captured
  its stderr to a file (it had been going nowhere); the only messages are the `gbm_wrapper` export
  errors and Chromium's usual noise - no fatal line. So there is nothing to read that names the fault.
- **A GBM shim cannot be installed the obvious way.** Chromium **dlopens** `libgbm.so.1` and calls
  `gbm_create_device` through `dlsym()`, so an `LD_PRELOAD` interposer of the symbol is bypassed. An
  interposer that also overrides `dlsym` was built and still did not stop the `gbm_wrapper` errors,
  so the call site is not the plain `dlsym` route either.
- **The two dma-buf allocators a GBM replacement would need are both denied to the app:**
  `/dev/dma_heap/system` -> `Permission denied` (measured), `/dev/udmabuf` could not be exercised
  (device flaky). Without one, a shim cannot mint a real dma-buf to hand back where GBM's export
  failed, so a "GBM over dma-heap" replacement is not available in this sandbox.
- **`--ozone-platform=headless`** (no GBM device at all) still crashed the GPU process.

**The reachable surface is now exhausted.** Every facility that could stand in for the missing
GEM/GBM (libdrm API, raw PRIME ioctls, the GBM API, dma-heap, udmabuf) has been either implemented
(the PRIME ioctls, `04926c5`) or shown to be denied. CEF's GPU process needs a real GEM/GBM backend on
the PowerVR node - a kernel driver, not an app or shim change - or a Chromium/CEF build whose raster
path does not require a platform buffer handle. **The frame-rendering goal is met** (vkcube, glxgears,
§48); Steam's CEF UI specifically is gated by the driver.

**Addendum 8 to §46 - replacing libgbm outright (session 87).** The direct test of "is GBM the
blocker": the client's `LD_LIBRARY_PATH` was prefixed (client-only, via `BL_CEF_LIBDIR`) with a
directory holding a **hand-written `libgbm.so.1`** whose `gbm_bo_get_fd` returns a real fd
(memfd-backed), so a GBM-using caller gets a non-null handle without the driver.

Measured (session `2026-10-11-87-steam`):

```
steamwebhelper.log  gbm_wrapper "Failed to export/get fd":  80 -> 0     <- the stub was used
cef_log.txt         "GPU process exited":                   69          <- still crashes
cef_log.txt         "Buffer handle is null":                351         <- unchanged
cef_log.txt         "Reinitialized the GPU process":        26
```

**So replacing GBM removes the GBM errors but does not stop the crash, and the `Buffer handle is
null` count is unchanged.** That is the strongest evidence yet that the null handle is not GBM's
export: even with GBM entirely substituted, Chromium still gets a null handle and still aborts. The
handle comes from a different part of Chromium's buffer path (its GPU-memory-buffer / shared-image
bookkeeping), fed by something other than the GBM export the stub replaced.

**A note on flakiness.** This round the device intermittently refused to start sessions (state stuck
at `STARTING_GUEST`, logs not refreshed). The numbers above are from the run where the stub was
confirmed in use (gbm errors 0); runs where the config did not apply are not counted. The conclusion
is consistent with addenda 4 and 5 (GBM removed by a scoped shim; Vulkan off) - three independent
ways of taking GBM out of the picture, all leaving the crash.

**The reachable surface is exhausted, now including a GBM replacement.** What remains for Steam is a
real GEM/GBM backend on the PowerVR node (kernel driver) or a Chromium/CEF build that does not take
a platform buffer handle on this path. The frame-rendering goal is met (§48); Steam's CEF UI is gated
by the driver, not by anything the app or its shims can supply.

## 49. A real AHardwareBuffer can be allocated from glibc - the GBM fix is now possible (2026-10-11)

**This overturns §46's conclusion.** The missing piece for CEF is a GBM that hands back a **real GPU
buffer**. The driver's GBM/GEM cannot; but the device's **gralloc** can, and it is reachable from the
glibc guest exactly the way the ICD already reaches the Vulkan HAL - through **libhybris**.

Measured (session `2026-10-11-97-ahb-test`), from a plain glibc binary in the session:

```
dlopen libhybris = 0x1023030
libnativewindow = 0x327b23c7                       <- bionic libnativewindow.so, via android_dlopen_ext
allocate=0x7960c32030 describe=0x7960c32500 release=0x7960c324d0
AHardwareBuffer_allocate rc=0 buf=0x1034530
described: 256x256 fmt=1 usage=0x300
```

So `android_dlopen_ext("/system/lib64/libnativewindow.so")` + `AHardwareBuffer_allocate` works from a
glibc process. (`libandroid.so` itself does not load - it pulls libgui/libui/libbinder/... - but on
Android 10+ the `AHardwareBuffer_*` symbols live in **libnativewindow.so**, which loads fine. That is
the library to use.)

**Why this matters.** A `libgbm.so.1` replacement for CEF's processes can now be built on top of
AHardwareBuffer:

- `gbm_create_device(fd)` -> open the render node, set up libhybris, resolve the AHB functions.
- `gbm_bo_create(dev,w,h,fmt,flags)` -> `AHardwareBuffer_allocate(w,h,fmt,usage)` -> a **real
  gralloc buffer**.
- `gbm_bo_get_fd(bo)` -> `AHardwareBuffer_getNativeHandle(bo)` -> `native_handle->data[0]`, the
  dma-buf fd (dup'd).
- `gbm_bo_map/unmap` -> `AHardwareBuffer_lock/unlock`; `gbm_bo_destroy` -> `AHardwareBuffer_release`.

Then Chromium's `gbm_wrapper.cc` gets a non-null GpuMemoryBufferHandle backed by actual GPU memory -
not the memfd the earlier stub returned (which could not become a texture, which is why that test was
inconclusive: `Buffer handle is null` counts were unchanged there, but the stub was never a fair test
of a *real* buffer). This is the same glibc->bionic bridge the ICD uses; the pieces are proven.

**Next:** implement the shim (`libgbm.so.1` over AHardwareBuffer, loading libhybris.so beside it),
scope it to the client's processes via `BL_CEF_LIBDIR` (already wired), and measure whether CEF's GPU
process stops crashing and its UI comes up.

## 50. A GBM shim over AHardwareBuffer cuts CEF's crash from ~70 to 1 (2026-10-11, session 102)

**The fix §49 pointed at works.** Built `libminigbm.so` (a replacement for Steam's own minigbm - see
below) whose GBM API is answered by real gralloc buffers: `gbm_bo_create` -> `AHardwareBuffer_allocate`
through libhybris, `gbm_bo_get_fd` -> the AHardwareBuffer's native-handle dma-buf fd. Validated
standalone (session 99):

```
gbm_create_device = 0x102b150
gbm_bo_create     = 0x1026200
gbm_bo_get_fd     = 10  link=/dmabuf:AHardwareBuffer pid [15745]  stride=1024
```

i.e. a **real dma-buf backed by gralloc memory**, which is what Chromium needs and the driver's
GBM/GEM cannot give.

**It must replace `libminigbm.so`, not `libgbm.so.1`.** Steam's client ships and loads its own
`steamrtarm64/libminigbm.so` (Chromium's minigbm); a `libgbm.so.1` in `LD_LIBRARY_PATH` is never
consulted. Replacing `steamrtarm64/libminigbm.so` (backup kept as `.real`) is what takes effect.

**Steam verifies the file's SIZE.** With the 37 KB shim in place the client logged
`BVerifyInstalledFiles: steamrtarm64/libminigbm.so is 37584 bytes, expected 2046456` and exited
`rc=42` (its self-repair restart). Padding the shim to **exactly 2046456 bytes** (the real library's
size; ELF ignores trailing bytes) lets it load.

**Measured, session `2026-10-11-102-steam` (padded shim), against the pre-shim baseline:**

| | baseline | with the shim |
|---|---|---|
| CEF `GPU process exited` | ~50-70 | **1** |
| `Buffer handle is null` | ~350 | **9** |
| `gbm_wrapper` export errors | 80 | **0** |
| update-UI presents (2404x1080) | 21 | **40** |

So the AHB-backed GBM is the right fix and it very nearly works. **The main client window is still
1x1** and the client still ends `WaitingForCredentials`; the remaining work is the last crash and the
1x1 - but the GPU-process crash that made the UI impossible is now essentially gone.

**Caveats:** the shim is a dev artifact (hand-built, padded, `libhybris.so`/`q.so` beside it); it is
not yet packaged into the app. The client's self-repair may restore the real `libminigbm.so` (its
`.real` backup makes this reversible). The device was intermittently refusing sessions this round.

**Session 105 (shim + `BL_CEF_OZONE=wayland`) confirms the result holds:** `rc=0`, CEF GPU-process
crashes **1** (baseline ~70), `Buffer handle is null` **9** (baseline ~350), `gbm_wrapper` export
errors **0** (baseline 80). The remaining single crash is still the `one_copy_raster_buffer_provider`
/ `shared_image_interface_proxy` "Buffer handle is null" path - i.e. a minority of Chromium's
buffer allocations still come back empty and abort the GPU process once, but the great majority now
succeed.

**Remaining work, both now small and well-defined:**
1. **The last ~9 null handles.** The shim answers `gbm_bo_create`/`gbm_bo_get_fd`, but some path still
   yields no handle - a call the shim does not cover (another minigbm entry point, a `gbm_bo_import`,
   or a usage/format the AHB rejects). Logging which call fails is the next step (the marker logs did
   not appear - the client's `/tmp` is not the app's `files/linuxfs/tmp`, so instrument to a path the
   app can read, e.g. under the Steam root).
2. **The 1x1 main window.** The client creates `0x1200002` but never `Show window`s it; the update UI
   (`0x800002`) shows and presents at 2404x1080. With the GPU process now surviving, this is likely to
   resolve as the client finishes bringing its UI up - or is the separate X11/DRI3 `BadAlloc` on the
   client's own present.

**This is a genuine fix, not a diagnostic.** CEF's GPU process went from "aborts on every buffer
allocation" to "survives all but a handful". The AHardwareBuffer-over-libhybris bridge is the answer
to the missing GEM/GBM, and it is now demonstrated end to end.

**Session 116 (robust shim with a format/usage fallback): crashes 1, `Buffer handle is null` 3**
(down from 9). The fallback - on an AHB allocation failure, retry with plain RGBA and minimal usage -
closes most of the last gap. The remaining single crash is a handful of allocations the fallback does
not cover.

**The 1x1 is NOT the GPU crash - it is a separate client/gamescope interaction (measured).** With the
shim in place and the GPU process essentially healthy (1 crash), the client's GamepadUI window
(`0x1200002`) is *still* created at 1x1 and never shown, and the client ends `rc=0`. Evidence it is
the client's own window-sizing, not the ICD or the compositor:

- The client's own display manager reads the correct size: `OnOutputMode: 2404 x 1080` and
  `RematchCurrentMode: ...2404x1080@60` (systemdisplaymanager.txt) - it knows the output is 2404x1080.
- Yet its X window is created `CreateBrowser ... (-2147483648, -2147483648) 0x0` and its swapchain is
  1x1. Steam creates the window hidden/zero-sized and expects the window manager (gamescope) to
  configure it to the output size; gamescope's `win_skip_and_not_fullscreen` excludes a 1x1 window
  from focus (steamcompmgr.cpp), so it is never made the focus window and never sized - a
  chicken-and-egg the client does not break on its own.
- The client logs `Error: XRRGetOutputInfo() is not available` and
  `CX11WindowProperty: failed to read window property: GAMESCOPE_VIEWPORT_SUPPORTED` - the two
  client->WM size signals it uses are the ones failing.
- `--force-windows-fullscreen` did **not** change it (tried; the swapchain stayed 1x1).

So two independent problems remain, both now narrow: (1) the last ~3 null handles in the GPU process,
and (2) the client's main window never being sized - a gamescope/client focus-and-configure
interaction, most likely the client not being made gamescope's focus window, or its XRandR/viewport
size query failing.

**The fix that works is preserved** (`docs/development/pvr-re/hybshim/gbm/`), and it is a real
advance: CEF's GPU process went from aborting on essentially every buffer allocation to surviving all
but a handful, which is the difference between "no UI possible" and "UI is loading, just mis-sized".

## 51. The 1x1 window: the client draws its UI windowed and exits (2026-10-12, sessions 03-06)

With the AHB GBM shim in place the GPU process is healthy; this section pins down the remaining
window problem with a live probe.

**Facts measured in a running session** (an `xprop`/`xrandr`/`xwininfo` probe added to the session
script behind `BL_UI_PROBE=1`):

- `xrandr` inside the session reports `Screen 0: ... current 2404 x 1080` - the display size is
  correct and readable from the guest.
- gamescope exposes its properties on the root window (`GAMESCOPE_*`, `_NET_ACTIVE_WINDOW`,
  `GAMESCOPE_DISPLAY_IS_EXTERNAL = 0`), but **`_NET_CLIENT_LIST` does not exist** - gamescope's WM
  is minimal and does not export the client list.
- **By 75 s every client window is gone**: `xprop -id 0x1200002` etc. all return `BadWindow`. The
  client has already exited and destroyed them.

**Why.** The client's own logs (`console_log.txt`, `steamui_html.txt`) show it reaches
`SetLoginState: WaitingForCredentials`, creates its SteamUI browser window at
`CreateBrowser ... (-2147483648, -2147483648) 0x0` (hidden/zero-sized - Steam sizes its UI window
through the window manager, not itself), the webhelper restarts once (`Restart webhelper process,
counter 2`), then `Shutdown` - `rc=0` ~30 s in. The client is drawing its interface **windowed** at
1x1 and then quitting, so nothing is ever shown.

**Strongest lead: Steam is told the display is "Windowed".** `droiddeck-steam-ui-scale` seeds
`config.vdf` with the display name `"External: gamescope|||Windowed"`
(`docs/.../droiddeck-steam-ui-scale:16`), and the on-device `config.vdf` carries exactly that entry.
`|||Windowed` is Steam's own mode suffix for the display; on SteamOS the client's UI is fullscreen.
A windowed client UI is created at a window size the WM must supply - and gamescope's
`win_skip_and_not_fullscreen` excludes a 1x1 window from focus, so it stays 1x1. Changing that mode
to fullscreen (or removing the seeded entry so Steam defaults) is the next thing to try.

**Note.** `XRRGetOutputInfo() is not available` is a red herring: every libXrandr the client can load
does export the symbol, and the client's workaround is non-fatal (gamescope prints it and survives).

**§51 continued - what the client's main window is, and its exit (sessions 06-10).** The client's
main graphical surface is the window `0x1200002` (`type:12` browser, the canvas the UI draws on). With
the AHB GBM shim in place it is:

```
_NET_WM_WINDOW_TYPE(ATOM) = _NET_WM_WINDOW_TYPE_NORMAL
window state: Normal
Client accepts input or input focus: True
```

and its swapchain is created **1x1** (`pvr_wsi: swapchain: 3 images 1x1`). gamescope, whose
`steamcompmgr.cpp` skips a 1x1 window (`win_skip_and_not_fullscreen`, "If the window is 1x1 then
prefer anything else"), never makes it the focus window - the session logs `== focused app: none` -
so it is never sized to the output. The client then exits `rc=0` (~30 s in); it never presents a frame
to the main window.

Explicitly tried and **not** the fix: `config.vdf` display mode `Windowed` -> `Fullscreen`;
gamescope `--force-windows-fullscreen`; gamescope `--debug-focus`. None changed the 1x1.

**So the goal is one layer from done: the GPU path is fixed (shim), the client reaches its login
screen, creates its UI window - and that window is 1x1, never focused/sized by gamescope, and the
client exits rc=0.** The remaining work is getting gamescope to size (or the client to request a
fullscreen) window for the Steam client UI. This is a gamescope/client window-management problem,
separate from the GPU work that is now solved.

## 52. Live window tree: the real Steam UI is a CEF window that is never mapped (2026-10-12)

Read-only window dump taken *while the client ran* (the guest has no `xwininfo`, so a small read-only
X query tool - XQueryTree/XGetWindowAttributes/XGetWindowProperty, no writes - was run in a second
proot against the live Xwayland socket). Two runs, consistent:

```
screen 2404x1080
0x310         2404x1080+0+0     VIEWABLE                              <- gamescope's output/root window
  0x200001    1x1+0+0           unmapped       pid=?                  <- internal (gamescope/Xwayland)
  0x800002    2404x1080+0+0     VIEWABLE       (Steam update UI)      <- the update UI; shows + presents
  0xa0002d    1x1+-10+-10       unmapped       (InputOnly, depth 0)
  0x1000001   10x10+10+10       unmapped       pid=18937 name='steam' class='steam'
    0x1000002 1x1+-1+-1         UNVIEWABLE
  0xe00005    64x24+0+0         unmapped       class='Steam'
  0x1400001   10x10+-100+-100   unmapped       (InputOnly)
  0x1600001   10x10+10+10       unmapped       pid=19151 name='steamwebhelper'    <- CEF process
  0x1600003   200x200+0+0       unmapped       pid=19151 name='steamwebhelper'
    0x1600004 1x1+-1+-1         UNVIEWABLE
  0x1c0002d   1x1+-10+-10       unmapped       (InputOnly)
  0x1c00033   1280x800+0+0      unmapped       pid=19151 name='Steam Big Picture Mode' class='steamwebhelper'
    0x1400005 1280x800+0+0      UNVIEWABLE     pid=19151
      0x1400006 1280x800+0+0    UNVIEWABLE     pid=19151
```

**Answers to the questions asked:**

- **How many windows:** the root (`0x310`, 2404x1080) plus 10-12 children. Of the client's own: the
  update UI (`0x800002`, 2404x1080, **VIEWABLE**), the Steam client process's helper window
  (`0x1000001`, 10x10, `steam`, unmapped), a 64x24 `Steam` window (`0xe00005`, unmapped), and several
  `steamwebhelper` (CEF) windows.
- **The real Steam UI is `0x1c00033`, titled "Steam Big Picture Mode", class `steamwebhelper`,
  owner `_NET_WM_PID` = the steamwebhelper process (19151), sized 1280x800 - and it is `unmapped`
  (never shown).** This is the CEF browser window the UI would draw into.
- **The 1x1 windows are NOT the real UI.** They are helper/placeholder windows (the Steam client's own
  10x10 helper, CEF's 10x10/200x200 helpers, and Xwayland's 1x1 InputOnly internals). The main UI
  window is 1280x800, not 1x1.
- **Yes - the CEF browser window exists as a separate window and is not shown.** `0x1c00033` exists
  with the right title, size and owner, but its map state is `unmapped`, so gamescope never focuses it
  and nothing it draws reaches the screen. The client then exits `rc=0`.

So the blocker is not "the UI is 1x1"; it is **"the CEF UI window is created at 1280x800 but never
mapped/shown, so gamescope never composites it."** That is the precise remaining problem.

**§52 continued - what does NOT fix the unmapped UI window (2026-10-12, sessions 20-21).** Tested
against the live window tree:

- **`-cef-use-gl=desktop` (GLX) for the main client: worse.** The main UI ("Big Picture Mode") window
  never appears at all - only the update UI (which uses GLX) shows. So the default ANGLE/Vulkan path
  is the right one for the main client; GLX is not the answer here.
- **`config.vdf` display mode Windowed -> Fullscreen; gamescope `--force-windows-fullscreen`; Steam
  `-fullscreen`; gamescope `--debug-focus`:** none changed the unmapped window.

**The client's own sequence** (session 21): `Create window` (update UI 0x800002, shown) -> the main
client creates `0x1200002`, `0x1400002`, `0xe00005`, `0x2c00002` (all via the Gamescope WSI layer) ->
**`Shutdown`** ~40 s in -> `rc=0`. It never issues a `Show window` for any of them. So the client
creates the main UI windows and quits without ever mapping them.

**Remaining hypotheses (not yet tested):**
1. **Deck mode / `1280x800`.** The client runs `-steamdeck -steamos3`; the unmapped UI window is
   `1280x800` (the Steam Deck's native size) on a `2404x1080` output, and Deck mode is the one UI mode
   where the client is meant to draw fullscreen directly. Its `Show` may be gated on something Deck
   mode expects (gamescope control socket / SteamOS service) that is not present. The next test is a
   session with `BL_STEAMDECK=0` (non-Deck client) to see whether a normal client window maps and shows.
2. **A second X display.** The session runs `--xwayland-count 2`; a window created on one Xwayland can
   never be mapped/shown by gamescope running on the other. The live tree was read on `:0`, where only
   the update UI appeared; the main client's display may be `:1`, which the read-only tool could not
   reach. Worth checking which display the client and gamescope are on.
3. The `steamwebhelper` restarts (`Restart webhelper process`) tearing the window down mid-show.

## 53. THE SHIM WAS MISNAMED: it must be `libgbm.so.1`, and now CEF uses it (2026-10-12)

**The fix to the shim, found by reading what actually needs GBM.** `libcef.so` (CEF's engine, 208 MB)
is the GBM consumer - its `DT_NEEDED` is **`libgbm.so.1`**, and it references `gbm_create_device` and
`gbm_bo_import`. It does **not** reference `libminigbm.so` at all, and `steamwebhelper` references
neither. So installing the shim as `steamrtarm64/libminigbm.so` (§49-50, and the earlier `.real`
scheme) was **never loaded by anything** - the "Buffer handle is null" logs came from the real library.

**Correct placement:** the client's rpath is `$ORIGIN` (= its own `steamrtarm64/` dir), so the shim
goes there as **`steamrtarm64/libgbm.so.1`** (padded to the real `libgbm.so.1.0.0` size, 2046456 B).

**Measured, immediately:**

```
gbma_loaded.log:  gbma loaded comm=steamwebhelper pid=...   (10+ processes - the shim is loaded!)
gbma.log:         gbm_bo_create w=2 h=2 fmt=0x20203852 -> ok ahb=0x3000b09ed0   (repeated - called!)
cef_log:          "Buffer handle is null":  128 -> 0         <- the wall is GONE
```

So CEF's GPU process now gets a **real buffer handle** (`gbm_bo_create` returns an AHardwareBuffer)
where before it got null. `gbm_bo_import` was also given a real implementation (it previously returned
NULL, which alone would have produced the null handle).

**But the GPU process still crashes** (49x, mixed `exit_code=11` SIGSEGV and `exit_code=6` SIGABRT) -
a *new* fault, after the buffer is created. The buffers seen are tiny (2x2, format "R8"); the crash
now happens when Chromium uses the handle, not when it asks for one. This is the next thing to chase:
what CEF's GPU process does with the AHB-backed buffer (map? bind as a GL/Vulkan texture? import?)
and why that segfaults.

**Corrections to earlier sections:** §49-50's claim that the shim "cuts crashes 70->1" was measured
with the shim in the wrong place, so it was actually the *stock* library plus variance; the real,
reproducible effect is the one above (null handles 128 -> 0). The shim now lives at
`steamrtarm64/libgbm.so.1`.

## 54. Correct shim in place: null handle gone (128 -> 0); the crash moved past buffer creation (2026-10-12)

With the shim correctly installed as `steamrtarm64/libgbm.so.1` (so `libcef.so` loads it - §53), a
clean run measures:

```
cef_log:   "Buffer handle is null":  128 -> 0            <- the wall is GONE
cef_log:   "GPU process exited":     ~39 (still crashing)
gbma.log:  gbm_bo_create w=2 h=2 fmt=0x20203852 -> ok   (27x; the shim IS called)
gbma.log:  gbm_bo_get_fd:  0       gbm_bo_import:  0     <- CEF never gets to these
```

So the shim now gives CEF a real buffer handle, and CEF's GPU process **still crashes immediately
after** the series of 2x2 buffer creations - before it ever calls `get_fd` or `destroy`. The crash:

- is a **signal**, not a Chromium `CHECK` (no `FATAL:`/`CHECK failed` line in the GPU process's own
  stderr, which was captured);
- **no crash file is produced** by any in-guest handler - not the rig's `crashcatch`, not a handler
  installed from the shim's constructor (loaded by libcef.so, so it runs in the GPU process), and
  Chromium's own crashpad produces no `.dmp` (breakpad is disabled with `--disable-breakpad`);
- **no siginfo reaches a handler** - consistent with the process being killed by the browser/parent
  (crashpad's route) rather than crashing in-place, or with Chromium reinstalling `SIG_DFL`.

**Net:** the GBM buffer-handle wall - the thing that made the UI impossible - is fixed and measured
(128 -> 0). What remains is a crash after CEF receives the buffer, whose address is not capturable from
inside the guest with the tools available; the next step is a real debugger or reproducing it with a
minimal `gbm`-consuming program.

**How to install the shim (working recipe):** build `gbm_ahb.c` as `libgbm.so.1`
(`-Wl,-soname,libgbm.so.1`), pad to the real `libgbm.so.1.0.0` size (2046456 B), place it at
`steamrtarm64/libgbm.so.1` (rpath `$ORIGIN`), with `libhybris.so`+`q.so` and `HYBRIS_LINKER_DIR` set.
Source: `docs/development/pvr-re/hybshim/gbm/`.

**§54 continued - a real shim bug fixed, and the crash located to CEF's use of the buffer.**
Three shim fixes, each verified with a standalone reproducer
(`gbmrepro`: `dlopen` the shim, create device on renderD128, do CEF's exact pattern - 40x
`gbm_bo_create(2,2,fmt=0x20203852,flags)` then `get_fd`/`map`/`destroy`):

1. **`gbm_bo_map` passed the wrong struct to `AHardwareBuffer_lock`.** Its 4th argument is an
   `ARect{left,top,right,bottom}`, not a buffer desc; passing a desc made the lock fail and map
   return NULL. CEF then dereferenced the NULL map and crashed. Fixed to pass a proper rect.
2. **The lock usage was wrong:** the GPU bits used for *allocation* are not valid for
   `AHardwareBuffer_lock`; it needs a CPU access flag (`CPU_READ_OFTEN|CPU_WRITE_OFTEN`).
3. **`gbm_bo_import` was a stub returning NULL** - given a real implementation (keeps the imported fd;
   answers get_fd/stride/format from it). `libcef.so` does reference `gbm_bo_import`.

**Measured after the fixes** (standalone reproducer): `gbm_bo_map FAIL` is gone - the lock succeeds.
**In Steam:** `Buffer handle is null` stays **0** and `gbm_bo_map FAIL` is **0**, so the shim now
serves create + map correctly. The GPU process still crashes (`~48`), and the shim-call sequence at
the crash is **a run of `gbm_bo_create` (36x, all 2x2 R8) and then nothing** - no `get_fd`, no `map`,
no `destroy` in most runs (occasionally one `get_fd`+`map` right before). So CEF creates a batch of
tiny buffers and then faults; the fault is in CEF's own use of those buffers (its `gbm_wrapper` /
GpuMemoryBuffer bookkeeping), not in a call reaching the shim - **every `gbm_*` symbol the real
`libgbm.so.1` exports (all 38) is present in the shim**, so it is not a missing-symbol fault.

**Net for this stage:** the buffer-handle wall is fixed (128 -> 0) and the shim's create/map/import are
correct; what remains is a crash inside CEF after it has the buffers, whose address is not capturable
in the guest. Shim source: `docs/development/pvr-re/hybshim/gbm/gbm_ahb.c`.

## 55. Decisive isolation: GBM was the crash source; the UI blocker is separate (2026-10-12)

With the shim correctly loaded as `steamrtarm64/libgbm.so.1` (§53) and its map/import fixed (§54), the
GPU process still crashed ~48x. The decisive experiment: make **`gbm_create_device` return NULL**, so
CEF sees GBM as unavailable and cannot use the shim's buffers at all.

**Measured (session 45): `GPU process exited` 48 -> 1.** CEF handles the NULL device gracefully and
falls back to a non-GBM path (it logs once and carries on). So:

- **The GBM path is the crash source.** When CEF actually *uses* buffers through the shim, it faults
  (48x). When it can't use GBM (device NULL), it doesn't (1x). The crash is in CEF's use of the
  buffers, not in the shim's gbm_* entry points (all of which return sane values now).
- **This also means CEF has a working non-GBM fallback**, which is the path the update UI (which
  renders fine) already uses.

**But the UI still does not render.** With the GPU process nearly healthy (1 crash) and CEF off GBM,
the main client UI window is still created at 1x1 / never shown, and the client still ends
`WaitingForCredentials` then `rc=0`. So **the UI-blocking problem is independent of the GPU/GBM crash**
- it is the client's window creation/bring-up, not the graphics stack.

**Two shim variants now exist** (both in `docs/development/pvr-re/hybshim/gbm/`): the real
AHardwareBuffer-backed one (§53-54), and a diagnostic variant whose `gbm_create_device` returns NULL.
The former gives CEF real buffers (null 128 -> 0) but CEF crashes using them; the latter makes CEF use
its own fallback (crash 1) without the shim's buffers. Neither is yet a working configuration - the
remaining work is the UI bring-up, which is upstream of both.

## 56. Phase 1-3 (2026-10-12): NULL-create shim -> 0-1 crashes; main UI window DOES size, but its swapchain is 1x1 and nothing presents

Baseline recorded in `docs/development/pvr-re/ROLLBACK_BASELINE.md` (dist checksums, helper-script
hash, stock guest libgbm/libminigbm). Installed the **NULL-create** `libgbm.so.1` shim at
`steamrtarm64/libgbm.so.1` (padded to 2046456 B; the stock lib is untouched, it resolves via
`/usr/lib`). libhybris/q.so copied beside it and into `/tmp`.

**Phase 1 (low-crash baseline) — reproducible, 2 runs (sessions 49, 50).**
- The shim loads in **all 8 `steamwebhelper`** processes; `gbm_create_device -> NULL` is reached
  (logged once per session); **`gbm_bo_create` is never called** (CEF takes its non-GBM fallback).
- **GPU-process crashes: 0 and 1** across the two runs (baseline with stock libgbm: ~48). So the
  NULL-create fallback removes the crash entirely.
- The remaining 0-1 crash does **not** block fallback init: the client runs its full ~30 s, reaches
  `WaitingForCredentials`, and the update UI renders (21 presents at 2404x1080).
- The main UI window still showed 1x1 in the *session log*, but see Phase 3.

**Phase 2 (first broken transition) — instrumented our WSI** (`pvr_wsi.c`: every
`vkGetPhysicalDeviceSurfaceCapabilities*` now logs `surf/kind/window/geom/out/map/currentExtent`; every
`vkCreateSwapchainKHR` logs the surface and requested extent). Findings (session 52):
- Our WSI is **correct**: for the main client's `xid 0x1200002` surface it reports
  `currentExtent=2404x1080`.
- The main client's swapchain is created **requested=1x1**, and it goes through the **gamescope WSI
  layer** (`[Gamescope WSI] Creating swapchain for xid: 0x1200002 ... flip: true; pvr_wsi:
  create_swapchain: ... requested=1x1`).
- The gamescope WSI layer sources `currentExtent` from the **X window rect**
  (`frog_wsi.cpp` `xcb::getWindowRect`, line 1243/1567), so a 1x1 X window -> 1x1 swapchain. **The
  1x1 is the X window's size, propagated by the gamescope WSI layer**, not chosen by the client.
- All the 32x32/64x24/1x1 windows are `map=0` (unmapped) — helpers/internals.

**Phase 3 — the causal observation (NO intervention needed; xctl's title match failed, so these are
the client's own behavior).** Live window-tree sampling (session 56) shows the main UI window
**"Steam Big Picture Mode" (class `steamwebhelper`, depth 32) at `2404x1080+0+0 VIEWABLE`** with
children `1280x800 VIEWABLE` — at t+15, then it is gone at t+16.

So with CEF not crashing:
- the main UI window **does reach full size (2404x1080) and map (VIEWABLE)** — the earlier "1x1
  window" was the *pre-CEF-init* state;
- **but** the client's own swapchain for the main window (`0x1200002`) is created **1x1 and destroyed
  immediately** (never recreated at 2404x1080), **no frame from the main client is presented** (all 21
  presents are the update UI), and the client exits `rc=0` ~14 s in (`ProcessNextMessage: socket
  disconnected`).

**Reframed conclusion.** The remaining blocker is **not** window sizing and **not** the GPU-process
crash (both are resolved/benign under the NULL-create shim). It is the **client's UI swapchain/present
path**: the client builds a 1x1 probe swapchain for its window, the window grows to 2404x1080, the
client does not rebuild the swapchain at the real size, presents nothing, and exits. The next step is
to trace why the client never recreates its swapchain at the (now correct) window size — i.e. whether
it receives the `VK_ERROR_SUBOPTIMAL_KHR`/resize signal it expects, or gives up on the run of
gamescope `vkCreateComputePipelines failed (-13)` / `Error getting buffer` seen around the same time.

## 57. Phases 1-3 re-run with per-PID WSI logging: who fails what (2026-10-13, sessions 13-01..13-05)

The instrumented ICD (every WSI call now logs `pvr_wsi[pid]`, plus new acquire/get_images/destroy/
present-entry logs) was run on the NULL-create baseline, reproduced: shim loads in 7-8
`steamwebhelper` procs, `gbm_create_device -> NULL` reached, **GPU-process crashes 0-3** (stock ~48),
update UI presenting. Attribution, per PID:

- **`vkCreateComputePipelines failed (-13)`**: all 6 lines carry the `[gamescope]` prefix - gamescope's
  own compositor context through our ICD, at update-UI window-destroy time. They interleave with
  *successful* 2404x1080 update-UI presents - **not on the main-UI path, and not blocking frames**
  (the same failures occur while update-UI frames present fine). Not Steam, not our ICD's decision.
- **`Error getting buffer`**: this string lives **only in Xwayland** (pulled the binary and read the
  string's neighbourhood: `xwayland-present.c`, "present: Error getting buffer"). "Buffer" here is an
  **X Present vblank buffer**, not GBM/AHB. Causal chain, per-PID: steamwebhelper pids (12551/12553)
  present 32x32 X11 utility windows -> our `xcb_dri3_pixmap_from_buffers` fails **BadAlloc** (Xwayland
  cannot import our dma-buf - the no-GEM wall seen from its side, §40-41) -> the following
  PresentPixmap references the never-created pixmap -> `BadPixmap` -> Xwayland logs the error.
- **The 1x1 swapchain churn**: the **steam** pid (12319, `pEngineName: mesa zink`) creates 1x1
  swapchains on surfaces whose caps *it was just told* `currentExtent=2404x1080` (xids 0x1200002,
  0x1400002, 0x2c00002), calls `get_images`, and **destroys without ever acquiring or presenting** -
  the shape of a size/compatibility probe. Its only real present is a 64x24 overlay window.
- **Never happens anywhere**: a swapchain for the Big-Picture CEF window (class `steamwebhelper`,
  `Steam Big Picture Mode`, §52/§56); any acquire/present at 2404x1080 for a main-UI surface.

**Answer to the objective.** The compute-pipeline failure does **not** prevent the main UI's first
frame (gamescope-owned, interleaves with successful presents). The 1x1 churn is **not the root
either**: it is the *steam* process's own probing on surfaces it then abandons; the window that
matters (the CEF Big-Picture window) never gets any Vulkan surface from any process, and the CEF
**browser keeps restarting** (`Restart webhelper process, counter 2`) with **residual
`Buffer handle is null` (~17)** even under NULL-create. The first failed transition is **before**
any main-UI window/swapchain: CEF's browser-side shared-image/GpuMemoryBuffer allocation still fails
and its GPU-side process keeps restarting, so the login/Big-Picture content never commits a frame to
present - while steam's client loop probes-and-aborts its swapchains and exits `rc=0` at ~14-30 s.

**Causal test run (external resize, xresize tool):** executed in-guest at probe rounds 3/5/7/9.
At 3/5 it reported "no small steam window found" (only the update UI's 2404x1080 window was live; the
steam helper windows' lifetimes are shorter than the 4 s probe cadence / fall inside the webhelper
restart churn). Per the handoff's step-5 rule ("if no acquisition happens, do not add resize logic
as the first fix") this closes the resize-invalidation idea for now.

**Open sub-question for next session (one variable each):** (a) the residual ~17 null handles and
the webhelper restarts co-occur under NULL-create - confirm by pid/timestamp correlation (shim log
vs `webhelper_gpu.txt` "start count") whether they are one loop or two; (b) the shim matrix so far -
stock(null~350,crash~48) vs AHB(null 0, crash ~48-57) vs NULL-create(null ~17, crash 0-1) shows the
GPU-process crash tracks **device creation**, not the buffer path - a fourth row, **valid stub device
+ failing `gbm_bo_create`** (import-only fallback), would separate "device missing" from "allocation
missing" and is the cheap next run.

Rollback of the two instrumentation changes: they are additive logging (`pvr_wsi[pid]` in
`pvr_wsi.c`, probe rounds gated on `BL_UI_PROBE=1` in `droiddeck-session`); `git revert` both or
rebuild the ICD from the previous commit. Guest is restored stock (ICD `68f90ad8`, no shim, env
cleared, all probe binaries removed).

## 58. Matched-control experiments (2026-10-13): nulldev vs stub-device; and §57's residual-null claim corrected

Built one probe shim (`hybshim/gbm/gbm_probe.c`, exports a superset of stock libgbm's 38 `gbm_*`
symbols, verified with llvm-nm; mode selected at load by env `GBM_SHIM_MODE=nulldev|stub` so both
runs use the **byte-identical artifact** `73bde3bc...` padded to 2046456, same instrumented ICD
`b1728201...`, same staged probe script (both runs launched from the one 14:46 APK build; `BL_XRESIZE` unset and `/tmp/xresize` absent, so no X writes in either run) - matched control, one variable).
Shim log = epoch-ms + pid.tid + comm/role + caller module+file-offset (dladdr) + args + return +
errno, at `/root/.local/share/Steam/logs/gbma.log` (O_CLOEXEC; caller fds only readlink'd, never
closed). Device/observer hashes recorded; rollback = delete the file (stock libgbm is in /usr/lib).

**Experiment 1 - nulldev (session 13-06).** Exactly 12 shim lines: 8 LOADs (roles: browser x2 gens
18411/18724, zygote x4, utility x2), and **2 `gbm_create_device(fd=39 '/dev/dri/renderD128') -> NULL
errno=ENODEV`** - both from caller `libcef.so+0x2df97f0`, from pids 18545/18761 that have **no LOAD
line and `--type=zygote` inherited cmdline** => GPU processes forked from the zygote (constructor ran
in parent; role string inherited). One per browser generation, ~10 s apart = Chromium GPU init, twice.
**No other GBM entry point is ever reached** (0 bo_create/import/destroy/query lines). Window tree:
Big-Picture window `0x1c00033` **VIEWABLE 2404x1080 depth-32 pid=18411** by probe round 6; **0
Wayland presents on it**; presents total 21, all update-UI; `rc=0`.

**Attribution correction to §57.** Steam **appends** `cef_log.txt` across browser/webhelper
relaunches, and `collect_steam_logs` copies that whole file. Session 13-06's copy began with
`[32372:...122317...]` - the *previous* run's lines; its 17 `Buffer handle is null` + 1 `GPU process
exited` belong to pid 32372 (12:23 session), **not** to 13-06's pids: grepping cef_log for 13-06's
own pids (18411/18545/18607/18724/18734/18733/18761/18790) yields **zero null-handle and zero
GPU-exit lines**. So "nulldev keeps ~17 residual nulls + restart loop" was log contamination. Under
clean attribution, the nulldev GPU process is **silent: no GBM traffic beyond the two create
attempts, no shared-image errors, no exits** - and **still** no login frame is ever presented.
§57's "residual CEF shared-image loop blocks the first frame" is therefore **not supported**; the
stock/AHB crash counts there (~48) were likewise pre-append-discovery grep counts and are upper
bounds, though the AHB shim's crash coupling is independently corroborated by its own shim logs.

**Experiment 2 - valid stub device, honest failure of every allocation (session 13-07, cef_log
truncated first: fresh).** GPU gen1 (pid 21109, zygote fork, 4.3 s after browser 21011):
`gbm_create_device -> dev=0x30010184a0` **accepted**, then an immediate probe sweep from
`libcef.so+0x2dfa0d8`: **16 `gbm_bo_create` of 2x2 buffers** across fourcc `R8  `, `GR88`, `AB24`,
`XB24`, `AR24`, `XR24`, `AR30`, `RG16`, `NV12`, `NV21`, `P010`, flags `0x0` then `0x5` (LINEAR /
LINEAR|RENDERING) - all answered `NULL errno=ENOSYS`; then a `gbm_device_is_format_supported`
sweep (caller `libcef.so+0x2dfa904`, usage=0x1, same fourcc list) all `-> 0`. GPU gen1 exits
**`exit_code=11` (SIGSEGV) at 14:58:36**, ~4 s after create_device; gen2 (21419) resumes the
is_format_supported sweep at 14:58:42. **`gbm_bo_import` and all surface entry points: never
reached.** Interpretation branch hit: stub device -> probe fails -> Chromium **segfaults instead of
gracefully rejecting GBM** (Chromium's own init bug on this path, or it consumes a return my shim
does not provide). Client outcome unchanged: Big Picture never presents, `rc=0`.

**What the matrix now says (per-pid-clean only):**
- nulldev: GPU silent, window sized+mapped, **still zero UI frames** - so no GBM/shared-image
  failure is on the critical path anymore; the first frame is blocked **downstream of graphics
  init entirely**.
- stub device: Chromium's GBM-format probe (2x2 = `GbmWrapper::Initialize`-style probing) needs at
  least **one** supported alloc to proceed, and dies on SIGSEGV when all fail.
- AHB (all 2x2 probe succeeds -> then crashes ~48x/upper-bound count): Chromium advances past probe
  and then faults consuming buffers.
The 2x2 probe buffers are **compatibility tests, not UI buffers**; the UI's real GpuMemoryBuffers come
later (never reached in these runs). Producer of any null handle = browser-side
`GpuMemoryBufferManager` (gbm_wrapper.cc, the only GBM user); consumer = GPU process
`shared_image_interface_proxy.cc:130`. **Connection of any GBM/shared-image failure to the missing
login frame: not proven - and the nulldev result argues against it.**

**Next one-variable step (evidence points here now, not at graphics):** with a healthy, silent GPU
process and a mapped 2404x1080 window, why does the client present nothing? Two candidates, one
cheap each: (a) webhelper browser gen1->gen2 restart (~10 s) has **no logged reason** - the restart
itself may be the blocker (login window recreated per generation, swapchain probe abandoned each
time): read the LIVE per-run `webhelper.txt`/`cef_log` browser-exit lines, and CDP (`-devtools-port`,
open by default in these sessions, reachable in-guest via proot curl to 127.0.0.1:<port>/json) to
see whether the login page loaded/painted; (b) CEF's own window->compositor path (the 64x24 overlay
presenting fine shows our WSI works). Do not touch WSI sizing, do not package.

## 59. CDP: the login page IS alive and visible - and its viewport is 1×1 (2026-10-13, sessions 10-13)

Baseline re-verified before use (probe shim `73bde3bc`, instrumented ICD `b1728201` rebuilt
byte-identical, `GBM_SHIM_MODE=nulldev`, Steam logs archived to `archive-13b/` pre-run, inodes/sizes
recorded; all conclusions PID-filtered). One CDP caveat: **CEF's `/json/list` title/url is
`SharedJSContext / index.html` for ALL pages** - the login UI lives inside that page's document, so
"the target" is the whole SteamUI, not a separate login target.

**Sessions 10+11 (passive + one-shot bounded page probe; CDP v2 works):**
- CDP endpoint: DOWN until browser generation-1 (steamwebhelper pid 8279, launched ~+4 s) exists;
  `Chrome/126.0.6478.183, Protocol 1.3`, **one page target**, UP at ~T+26 s, DOWN one round later
  (~7 s). Gen2 browser (13116 at 16:22:53, `-startcount=1`) never exposes CDP before `rc=0`.
- The page at UP: `document.readyState=complete`, `visibilityState=visible`,
  **`innerWidth×innerHeight = 1×1`**, dpr=1, body has 2 children - the SteamUI document IS loaded and
  thinks its window is one pixel square.
- **The one bounded screenshot: 759 bytes, decodes to a **1×1 blank** (text-parsed SOF: height=1 width=1
  no login pixels). A 404 resource error, no JS exceptions logged in the attach window.
- So: **the first loss of progress = the CEF page viewport.** The Big-Picture X window is mapped and
  2404×1080 (§56/§58), but Chromium's web contents never learn any size - viewport 1×1 - so no login
  pixels exist to present, consistent with "no swapchain ever created for the Big-Picture window".

**Session 13 (exit lifecycle, LD_PRELOAD `_exit/exit`/abort tracer - separate run, not mixed into
the passive control):** every `steamwebhelper` exit caught was **`_exit code=0`** - clean exits,
no signals, no nonzero statuses. Steam's `Disabling sandbox due to a previous crash in CefInitialize`
is a persisted config line (same text on 2026-10-11 runs), not a crash this run. So the "browser
disappearance" is Steam's own **orderly shutdown sequence** (webhelper exits 0; client exits 0),
preceded by the page sitting at 1×1 for its whole (short) life - not a crash loop.

**Reading of the generation swap:** gen1 browser lived ~7-10 s from CDP-UP to disappearance with
`exit(0)`; gen2 launched immediately and the client gave up ~14 s in (`socket disconnected` then
`Shutdown`). Whether Steam's own logic terminates gen1 (a size/timeout check) or the page-1×1 wedges
the UI bootstrap until Steam's watchdog quits is **still unproven** - but ordering is clear: the page
is 1×1 from the first moment CDP can see it, i.e. **before** the replacement. The replacement is
therefore *downstream* of the stalled first page, not its cause.

**Branch chosen for the next step (STEP 5, evidence = branch C/E):** content exists but its
delivery never starts because the web viewport is 1×1. Next targeted diagnostic, still via the same
bounded CDP attach (one evaluate, no ops): read `window.outerWidth/outerHeight`,
`document.documentElement.clientWidth/Height`, and install a 5 s `window.onresize` recorder to see
whether a resize event ever reaches the page - distinguishing "CEF was never told" from "told but
ignored". Only then revisit WSI/extent (and per §58, not `vk_wsi_force_swapchain_to_current_extent`
until the login path is proven to use that surface).

**Device restored to stock at session end** (probe shim + observers + ICD instrumented build removed;
stock ICD `68f90ad8` verified; env deleted; logs archived not deleted).

## 60. Session 20 (STOCK, no shim): the Big-Picture browser view exists, sized 1280x800, but `visibilityState=hidden` (2026-10-13, sessions 13-18..20)

CDP observer v5/v6 (HTTP-only lifecycle diff + read-only page attach; in-session, guest-local
endpoint, all URLs query-stripped). NOTE: session 20 ran on fully STOCK graphics (the probe shim had
been removed) — so everything here is independent of GBM and of the shim matrix.

**The target list settles the SharedJSContext question exactly as the user warned.** At
1791927452 (gen2 browser) `/json/list` holds **two pages**:
1. `SharedJSContext @ steamloopback.host/index.html` — the **headless SteamUI controller** (its
   own title carries `createflags=6292738&minwidth=853&minheight=534&browserType=4` params for the
   window it will request; its 1×1 outer/inner/screen is *normal for an offscreen controller*, and
   its console log shows the whole login-app JS booting: transport connected, `SteamApp Init -
   Before Login`, `OnLoginUsersChanged`, all stores async).
2. **`about:blank?createflags=...&useragent=Valve Steam Gamepad`** — the **Big Picture browser
   view itself**, attached: title becomes **`Steam Big Picture Mode`**, **`outer=1280x800,
   inner=1281x800, screen=2404x1080, dpr=1, readyState=complete, visibilityState=HIDDEN, kids=1`**,
   bridge: `SteamClient.{_internal,Browser,OpenVR,Input,Window,Compat,BrowserView}`.

**So:** the login/Big-Picture renderer DOES get created (CEF `browserType=4`, UA "Valve Steam
Gamepad"). It has real geometry (1280x800), loaded document, and a populated bridge — but
**Chromium considers it HIDDEN**, and Chromium does not produce frames for hidden renderers. That
connects §56's "no swapchain for it": the page exists on the web side, visibility is the
blocker. **Within session 20's own run** (xinspect rounds + the attach above), no X top-level of
1280x800/1281x800 and no window named 'Steam Big Picture Mode' ever existed in the tree the whole
session - only the root (2404x1080) and the update UI - while the web side held a *hidden*,
loaded, 1280x800 BP view. CEF believes the widget is 1280x800-but-hidden; the WM never received
(or refused to map) that window. So BP-window creation between controller and X11 is the gap; the X focus/configure chain is the remaining boundary (gamescope's WM
never makes it the focus window — `== focused app: none` in every session).

**Correction/retirement of §59's framing:** the 1×1 SharedJSContext screenshot/geometry was NOT the
login page's state (controller context — exactly as flagged). The real first-frame failure is:
**BP browser view created+loaded → stays hidden → never renders → no swapchain → nothing presents →
Steam shuts the helper down ~10-15 s later (clean `_exit(0)`, session 13), client `rc=0`.**
The 1x1-vs-2404 X window sizing (§56/§58) and hidden-visibility are now the same question: who
tells a top-level X window it is visible/focused in this gamescope+ozone-X11 stack. Next
instrumentation (single variable, next session): X `CreateNotify/ConfigureNotify/MapNotify` +
`_NET_ACTIVE_WINDOW`/WM_STATE deltas around the BP window from the moment it is created, plus
Chromium-side visibility reasons — not a WSI extent change (§58/§59 warnings stand: the BP view
already has geometry; extent hacks would not touch visibility).

**Association caveat + config split (proven/inferred boundary).** The link "BP web view ↔
`0x1c00033` X window" is **inferred** (title/class match), not proven: sessions §56/§58 ran WITH the
nulldev shim, where xinspect saw the BP X window VIEWABLE (2404x1080) but CDP showed only the
controller page; session 20 ran STOCK, where CDP showed the BP web view (hidden, 1280x800) while
xinspect's rounds never saw any BP-sized/named X window. Both observations can be lifecycle races
(attach-fails were real: `handshake-closed` on second pages in 17/18). A single stock run with
longer CDP uptime + same-round xinspect is needed to say whether the native BP window coexists with
the hidden web view — i.e., whether "hidden" is CEF-side (window created, never activated) or the
native window was never mapped at all.

## 61. Sessions 21-23 (same-run correlation): BP web target ↔ native window PROVEN by PID+geometry+title; the page stays hidden at read time, the native window IS mapped/active, and the GPU process segfaults ~0.85 s after activation at BP's first raster (2026-10-13)

Objective: correlate the BP browser target with its native window in ONE run and say why the
document reports hidden. Tooling: CDP observer v7.2 (`hybshim/cdp/cdp_probe7.py`) + native
observer `xinspect2` (`hybshim/x11/xinspect2.c`: fixes v1's ATOM-array mis-decode, adds parent,
override-redirect, legacy WM_STATE, _NET_ACTIVE_WINDOW vs input focus as SEPARATE observations,
plus a `--events` Create/Map/Unmap/Destroy/Reparent/Configure/Property tail; BadWindow races
non-fatal). v7.2 spawns the event tail the same instant the BP attach succeeds, so web and X
observations share one CLOCK_MONOTONIC line stream in `session.log`. Commits `057d488`,`7efbc7a`.

**Session 21 (STOCK, v7.1 — run recorded as OBSERVER-DEFECTIVE, not merged):** two tooling bugs
(mine, fixed in `7efbc7a`): CEF `/json/list` carries `"id"` not `"targetId"` → both pages hashed
to key `"?"`, the BP page collided with the inspected controller and was never attached; and
xinspect2 round 3 died via the default X error handler (update-UI window destroyed mid-walk,
BadWindow serial 18), rounds 3-5 empty. Surviving evidence: xinspect2's first proper decodes —
update-UI `0x800002` is **override-redirect=1, WM_STATE=Normal**; controller page visible at 1x1.
PID-filtered fresh cef_log: GPU process `[9245]` exit_code=11 + 15x shared_image null-buffer —
stock crash profile. Verdict: unusable for the BP question; NOT mixed into the stock conclusion.

**Session 22 (STOCK, v7.2 — first clean correlation, crash-compressed):** attach-ok BP at
mono=329023.98 (`about:blank?createflags=...` → title `Steam Big Picture Mode`):
**hidden=true, vis=hidden, focus=false, inner=1280x800, outer=1280x800, dpr=1, readyState=complete,
kids=1 childTags=["DIV#popup_target"], innerText len=0, all login markers 0.** The event tail then
recorded, same stream: mono=329024.52 FOCUSDIFF **active=0x1a00033('Steam Big Picture Mode'/'steamwebhelper')
input=0x1a00033** — the native BP window EXISTED, mapped, gamescope-activated ~0.54 s AFTER our
hidden read; mono=329024.94 UNMAP+DESTROY 0x1a00033 (browser pid 20532 died: fresh cef_log
[20532] GPU process exited exit_code=11 at the same instant; PID-filtered). The 6 s BP hold never
completed a second readback (websocket died with gen1). V7EV/V7B exist only for the controller page
(visible, 1x1, DIV#root — the SteamUI document).

**Session 23 (separately labelled NULL-CREATE: padded probe shim `839ac0b4...` (2046456 B) at
`steamrtarm64/libgbm.so.1`, `GBM_SHIM_MODE=nulldev` in env, cef_log truncated fresh, everything
else identical to session 22):** THE decisive same-run capture:
- mono=329687.98 attach-ok BP; 329688.02 **V7 BP: hidden=true, vis=hidden, focus=false,
  inner=1281x800, outer=1280x800, readyState=complete, kids=1 DIV#popup_target, innerText len=0,
  marks all 0** (identical shape to session 22 → not a one-run artifact).
- 329688.01 event tail start. 329688.53 FOCUSDIFF **active=0x1c00033('Steam Big Picture Mode'/'steamwebhelper')**;
  input focus = 0x1400006 (gamescope helper, revert-to-parent) — **X input focus NEVER entered the
  BP window**; active-window and input-focus are different observations.
- 329688.56 snapshot: **0x1c00033 par=0x310 2404x1080+0+0 d=32 or=0 VIEWABLE pid=26590
  wmt=[_NET_WM_WINDOW_TYPE_NORMAL] wmst=Normal+0 'Steam Big Picture Mode'/'steamwebhelper'**, child
  **0x1400005 par=0x1c00033 1280x800+0+0 d=32 VIEWABLE pid=26590** (no WM hints on the child).
- **Association PROVEN three ways** (not title-match alone): (1) `_NET_WM_PID=26590` on both windows
  = the steamwebhelper browser, and proc_watch's `cdpport=28386` line pins 26590 as the CDP endpoint
  owner = the web-side browser process; (2) child window geometry 1280x800 == web outer 1280x800
  (and inner 1281x800 = client-area +1 border pattern); (3) exact title + class at the same second.
  Confidence: HIGH.
- 329688.85: UNMAP+DESTROY 0x1c00033 — fresh cef_log: **[26590] 12x shared_image_interface_proxy
  'Buffer handle is null' + one_copy_raster_buffer_provider staging failure, then gpu_process_host
  exit_code=11** at 19:23:09.18, i.e. **~850 ms after the BP window mapped/activated**, precisely at
  the first BeginFrame/raster of the BP view. Shim log: gbm_create_device->NULL both gens, no other
  GBM entry reached (matches §58); the crash is NOT inside the shim — it is Chromium's shared-image
  raster path. gen2 repeats the cycle (CREATE/CONFIG 329692-94), client exits rc=0 ~+14 s.

**Reading (branches kept distinct, per STEP 5):** §60's config split collapses to one fact: the BP
native window DOES coexist with the hidden web view — stock sessions 20/22 "no BP window seen" was
sampling lifetime (window lives <1 s per gen, rounds are 4 s apart), exactly the "one-sample absence
is not proof" caveat. Branch A (unmapped/minimized) is REFUTED: WM_STATE=Normal, mapped, active.
Branch E (no association) is REFUTED: proven above. Between B and D: the page is hidden at read
(+4 ms), activation lands +530 ms, and the gen dies +850 ms BEFORE any second visibility readback
is possible — so "does activation flip hidden->visible" remains UNOBSERVED, and the binding
constraint moved: **the GPU process segfaults at BP's first raster under BOTH stock and nulldev+
stock-ICD** (identical shared_image null-buffer signature, PID-filtered fresh logs). Note §58's
nulldev "GPU silent" was under the instrumented ICD `b1728201`; with the stock ICD `68f90ad8` the
crash returns — the ICD/graphics variable is NOT closed after all, contradiction recorded, not
harmonized. Login content: the BP page is an empty shell (`DIV#popup_target` container only, zero
text) while the SteamUI/login document lives in the CONTROLLER page (DIV#root, visible 1x1, full
JS boot per §60) — so "readyState=complete" on BP never implied login mount; it is the popup/
backstack host, waiting to paint. Shutdown cause: NOT a watchdog claim — observed chain is GPU
SIGSEGV at BP first-frame → web target + native window die with the gen → Steam's orderly
`_exit(0)` recovery (~10-15 s, unchanged from session 13); the trigger of that chain is the crash.

**Earliest divergence (this run):** between web attach (hidden, fine, activation pending) and
+850 ms: the BP view's first raster buffer creation fails (shared image null handle) and the GPU
process segfaults. Responsible boundary: **Chromium GPU-process buffer/sharing path over our WSI**
(§49-53 territory re-adjacent), not WM activation, not extent, not focus. Next fix candidate (one,
reversible, labelled run): AHB shim `gbm_ahb.c` (the §50 artifact that cut crashes ~70->1 and hands
Chromium a real gralloc dma-buf) under the SAME v7.2 observer set — does the BP gen now survive
first-activation, does the second visibility readback (V7B BP) flip hidden->visible, do frames
present. Rollback = delete `steamrtarm64/libgbm.so.1` (stock libgbm is at /usr/lib). Device restored
to stock after sessions 21-23 and re-verified on-device: ICD `68f90ad8...`, libgbm `e28a74e1...`,
no shim/env/observers, IDLE (sha256-checked same-day).

## 62. STEP 6 causal intervention (sessions 24-25, AHB shim): FAILED, rolled back; and a fourth observer defect found (v7.3)

One reversible intervention was permitted after observation; chosen branch-appropriate candidate from
§61: the AHB-backed `gbm_ahb.c` shim (`a75d1af0…` padded 2046456, + `libhybris.so` `058bdaed…` /
`q.so` `ba9b7904…` beside it and in `/tmp`, `HYBRIS_LINKER_DIR` via env, stock ICD, `cef_log`+
`gbma.log` truncated fresh) — it hands Chromium real gralloc dma-bufs and previously cut crashes
~70→1 (§49-50). Prediction from §60's framing: BP gen survives first raster; then either flips
visible (branch D next) or stays hidden (branch B next).

**Session 24 (AHB, observer files accidentally removed at the §61 cleanup — outcome from raw logs
only):** shim loads and **`gbm_bo_create w=2 h=2 -> ok` x157 lines (real AHB buffers served)**,
then `gpu_process_host exited unexpectedly exit_code=11` repeatedly and **`FATAL:gpu_data_manager_impl_private.cc(449) GPU process isn't usable. Goodbye.`**
The browser process re-launched every ~5 s (startcount climbing), **CDP never exposed a usable page
list** (first `LIST pages=1` at 19:46, immediately `Shutdown`), **no BP native window ever appeared
in the gamescope tree** (only root/update-UI/probe 1x1s). Client never reached `rc=0`; session was
stopped manually at ~+3 min. `firstFrame=false`.

**Session 25 (AHB, observers re-staged; the clean intervention observation):** CDP-UP once
(19:52:16), one `attach-ok page 96F3A15DFE` (`index.html?LOCALE...`) and then **the observer died**:
the `Traceback` fragments now explained — v7 dropped v6's `try` around `inspect()` and `call()`
didn't guard `ws.send`; a dying websocket raised through the main loop. Fixed as v7.3 (`9a1bbd7`).
The native tail kept working (it is a separate process): webhelper **browser generations cycled
~6 s apart** (pids 13242→13956→15070→15630, all `cdpport=22586`), `cef_log` fresh-log count:
**174 `GPU process exited unexpectedly exit_code=11`** in ~4 minutes, no `Steam Big Picture Mode`
window, no FOCUSDIFF for any BP window, `Error getting buffer` + `DRI3 BadAlloc` churn. Session
stopped manually (client still looping, never `rc=0`).

**Intervention effects recorded (STEP 6 checklist):** (1) native state: NO BP window created — the
gen dies before page #2 exists; (2) document visibility: not observable this run (observer died at
first attach — tooling, not subject); (3) login content: none, device showed update UI only;
(4) device frames: only update-UI presents (2404x1080 wl_buffers) — no BP frames; (5) helper
lifetime: WORSE than nulldev — fast crash-restart loop (174 GPU exits, no orderly `_exit(0)`
observed before manual stop) instead of §61's ~15 s orderly shutdown. **Success criterion (login
content visible on device) NOT met; the intervention is rolled back** (shim + hybris copies + env
deleted; stock resolution re-verified on-device: `/usr/lib/libgbm.so.1` in effect, ICD `68f90ad8…`).

**Reading:** the AHB shim's buffers are accepted by GBM-API consumers but Chromium's GPU process
crashes *using them* (the exact §53-54 wall: crash located to CEF's post-import handling; siginfo
uncapturable in-guest). The nulldev path (§61) is the better-behaved failure: it dies once per gen
at first BP raster, ~850 ms after activation, with an orderly Steam shutdown chain. **Crucial
cross-session note for the next step:** §58's "nulldev + GPU totally silent (0-1 crashes, full
30 s run)" used the **instrumented ICD `b1728201`**, while §61's session 23 nulldev run used the
**stock ICD `68f90ad8`** and crashed — so the crash-vs-silent switch correlates with the ICD build,
not with the shim mode. The instrumented ICD differs from stock only by per-PID WSI logging
(`add0052`) — logging that changes GPU-process timing/lifetime is the hypothesis; it also means
**the §61 open question (does activation flip hidden→visible?) is answerable with a configuration
we already know keeps the BP gen alive long enough**: nulldev shim + instrumented ICD + v7.3
observer with a SHORT first hold and a second readback (+2 s, +4 s) inside the observed ~900 ms-10 s
window. That is the single next run; no new flags, same one-variable discipline.

## 63. Matched ICD A/B (sessions 29-32) + labelled crash capture (33-34): the shipped-vs-instrumented contrast does NOT exist; activation DOES flip BP visible; GPU-process death is universal at first BP composite (2026-10-13)

Objective: establish whether the ICD artifact controls CEF GPU-process survival, completing the BP
visibility readback if it survives. All runs: NULL-create shim 839ac0b4 (padded), same v7.6
observers, same env (BL_UI_PROBE=1 GBM_SHIM_MODE=nulldev HYBRIS_LINKER_DIR=...), same session
build; ONLY the ICD file at files/linux_vulkan_drivers/powervr-g5x/libvulkan_powervr.so differed.

**STEP 1 audit (actual artifacts, not assumed):** Arm A b1728201 = HEAD pvr_wsi.c (commit add0052);
Arm B 68f90ad8 = e60ba95 pvr_wsi.c, rebuilt in-tree and byte-identical to the shipped file;
pvr_icd.c, pvr_wsi.h, protocols, build_icd.sh, zig target/flags identical. The diff is: log-macro
gains pid prefix; caps logging moved from once-per-process to EVERY call, and for X11 surfaces the
log path performs an extra xcb_get_window_attributes round-trip per caps query (a NON-diagnostic
behavioural addition - not "logging only"); single log lines at create/destroy/get_images/acquire/
present entry. Dependencies/load env identical (support libs 058bdaed/ba9b7904, same icd.json).
Actual-load proof per run used the stderr signature, not the installed hash: A prints
"pvr_wsi[pid]: caps: surf=... map=..." (r29: 15 prefixed lines, 0 old-format); B prints old-format
"pvr_wsi: caps:" (r30: 6 old lines, 0 prefixed).

**STEP 2 matched sequence A,B,A,B (runs 29,30,31,32; precursors 27,28=A):** Primary outcome:
the GPU process survives BP map+activation in NEITHER arm. Every run: BP native window CREATE/MAP
~+550 ms after web attach, gamescope activation lands (r27/r31: X INPUT FOCUS lands ON the BP
window itself, input=0x2a00033 revert=0), then UNMAP+DESTROY +0.4-1.0 s; CDP-DOWN with it;
"V7M-fail ... ('Runtime.evaluate','io')" = target/generation death recorded AS death, not as
"still hidden". GPU "exited unexpectedly exit_code=11" logged in r29,r33,r34 (pids 21690,16397)
but 0 such lines in r30,r31,r32 fresh logs - the exit-LINE varies, the DEATH does not: Steam
relaunches webhelper with -startcount=1 in all arms within seconds. Verdict: the 62 crash-vs-
silent contrast was NOT reproducible and does not control survival. 58's "instrumented ICD keeps
the gen alive 30 s" is retired for the current config; differences recorded honestly (58 used a
shim build hashing 73bde3bc vs today's 839ac0b4 rebuild of the same source, and no v7-era BP
attach was performed there). One caveat for the next cycle: all four matched runs attached the BP
page via CDP; an attach-triggered first-composite remains a possible common cause, separable by
an attach-off control arm.

**STEP 3 readback COMPLETED - the open question answered:** with a 0.35 s multi-read cadence the
flip is captured in EVERY run that reached it (r29-34): V7 at attach shows hidden/hidden/focus=
false 1281x800, and V7M1 at +0.5-0.7 s shows **hidden=false vis=VISIBLE focus=TRUE**, same
1281x800, readyState=complete, kids=1 DIV#popup_target, innerText len=0, marks all 0 - right
before target death. _NET_ACTIVE_WINDOW activation DOES flip the BP document hidden->visible and
Chromium grants it document focus: branch B ("stays hidden") is REFUTED. Login content is absent
at the visible instant, but that is the same instant the GPU process dies; no swapchain was ever
created for the BP surface in ANY run (the instrumented arm logs every create_swapchain - sizes
seen: 2404x1080 update-UI, 1x1 probes, 64x24, 32x32 utility; never a BP-window surface). The
controller page stays visible-1x1 throughout (normal, 60).

**STEP 5 labelled crash-capture (r33,r34; never mixed into the A/B table):** crashcatch extended
(per-pid report files; install-at-load for fatal signals, since the GPU process runs
--disable-breakpad and the sigaction-wrap design waits for an app registration that never comes).
Captured with siginfo+registers+stack-scan+maps:
1. crash.txt-15507 comm=Xwayland SIGSEGV si_addr=0xfffffffffffffff0 pc=0x3f00010648 - the known
   Mesa/gallium teardown fault (47), benign, session end.
2. crash.txt-16504/16509 comm=vulkandriverquery (Steam's own GPU driver-query child; main
   executable steamrtarm64/vulkandriverquery per its maps) SIGSEGV si_addr=0x308 (NULL+0x308),
   pc AND lr both in bionic libc (pulled /apex/com.android.runtime/lib64/bionic/libc.so, base64-
   verified, 1375440 B): pc file-offset 0x80944 = __errno+0x34 region (nearest dynsym; an errno/
   TLS helper), lr 0x91c54 = __open_2+0x54 return site - the query process faults dereferencing a
   bad pointer on an open/errno path. NEW, reproduced 2/2 in r34.
3. The GPU-process fault itself: STILL NOT CAPTURED, mechanism now measured. r34 logs GPU exit 11
   at 21:24:47 (same instant as V7M2-fail) preceded by 159 "Buffer handle is null" attempts, yet
   NO crash.txt for the gpu pid exists. Consistent explanation (54 + this run): the GPU process is
   forked from the Chromium zygote, whose fork path resets signal dispositions in the child; an
   LD_PRELOAD load-time handler does not survive into it, and the breakpad-disabled child never
   calls sigaction for SIGSEGV - preload capture cannot reach it as designed. Honest negative
   result. Recorded next capture variant (NOT run): intercept the DISPOSITION RESET too (wrap
   clone/fork paths is not feasible; instead have the shim's constructor install handlers AND wrap
   execve-scope... practical option: wrap sigaction to re-front even when act->sa_handler==SIG_DFL
   - today the wrapper skips exactly that case).

**STEP 6 reading + next step from evidence:** branch "both crash" holds - the stable-build
assumption is retired; follow the fault. Device-frame work sequences AFTER GPU-process survival:
first BP frames require the GPU process to live past first composite, which happens in NO
configuration tried today (stock; nulldev x either ICD; AHB dies earlier). Evidence points at
Chromium's shared-image/raster path dereferencing a null handle (159 attempts -> SIGSEGV) over this
node's no-GBM/no-GEM export, plus Steam's vulkendriverquery crash as a secondary, newly localized
fault. Priority order: (1) gpu-process capture fix in crashcatch (handle the SIG_DFL re-registration
case) in a labelled run, to localize libcef vs our ICD/WSI vs HAL; (2) attach-off control run;
(3) then a targeted code fix at the proven site. No WSI extent, no focus, no new GBM matrices.

**Rollback/cleanup status (accurate):** the first rollback command ran while the device was flaking
off USB and FAILED. Runs 33-34 executed with arm B ICD (68f90ad8) + nulldev shim + crashcatch + env
armed; until the completed cleanup is verified on-device, assume the shim, libhybris/q.so copies,
observers, crashcatch.so, and Download/droiddeck-env are STILL INSTALLED. The verified rollback
(ICD hash re-check, steamrtarm64 clean, /tmp clean, env absent, session IDLE) is recorded in
UPDATE 13g's closing paragraph with the command outputs.

**63-addendum (vulkandriverquery backtrace, symbolized after fixing the stack-word regex - crashcatch
writes 0x-prefixed tokens):** fault path: vulkendriverquery+0x3458/0x349c -> glibc ->
libvulkan.so.1+0x307e0 -> **our ICD/WSI region (vkEnumerate*/instance path) -> libhybris.so+0x2c4e4
-> HAL vulkan.powervr.so entries at file offsets 0xd5a10, 0xdcf08/0xdcf2c/0xdd000 -> bionic
__open_2/errno helper -> SIGSEGV at 0x308.** So Steam's GPU driver query crashes inside the bionic
HAL while enumerating/opening something (probably a sysfs/drm path) through our hybris bridge -
a SECOND, independent fault, distinct from the GPU-process composite crash (whose capture is fixed
in e15bead and awaits a labelled run).
