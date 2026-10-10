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
