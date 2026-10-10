# Google Tensor support — reconnaissance and change map

Status: **pass 4 done — code changed.** Branch `feature/google-tensor-support` of the fork
**`Fizzolas/DroidDeckTensor`**, based on `Droid-Deck/DroidDeck` @ `9310d19` (versionName 0.3.1,
versionCode 11). Remotes: `origin` = the fork, `upstream` = DroidDeck. Nothing committed or pushed.

Agreed direction (see §5): **full support for Google Tensor G4/G5 (Mali-G715), including a bundled
driver.**

Passes so far:

* **Pass 1 (§1-§5):** reconnaissance and the change map below. No code.
* **Pass 2 (§6-§7):** the first code slice — a Tensor/Mali GPU is recognised *as Mali* (kbase, no
  KGSL) instead of `UNSUPPORTED`, and Auto no longer hands AdrenoTools an Adreno Turnip on a GPU that
  is not an Adreno. The guest-side driver artifact and bundling are deliberately **not** in it; §7
  records exactly where that slice hooks in.
* **Pass 3 (§8):** consolidation. One owner for the Mali wording and the Tensor codename table, the
  detection probes collapsed to one function each, `DeviceSupport` removed, the explicit refusal of a
  pinned Adreno driver on a Mali, and this doc's stale references removed.
* **Pass 4 (§9-§11):** the driver seam and the guest ICD route — the critical path, worked in the
  order that gets a device furthest. One vendor-aware display pick that only ever offers a build the
  apk actually carries, the guest's glibc driver import opened from Turnip-only to any vendor
  (`libvulkan_*.so`), and the runtime's ICD discovery and the Flatpak GPU plan keyed on the device's
  stack. The one artifact that is still missing is named once in §10, and §11 writes down what
  "working on Mali" will be judged by.

---

## 1. What this project is

DroidDeck is an Android app that runs **Valve's Steam client (Big Picture) and Windows games** on
Android. It is not an emulator of the OS: it runs an **Arch Linux ARM rootfs** (the "Linux runtime",
downloaded on the device, not stored in this repo) under **proot**, with its own **Wayland
compositor** built into the app process, and games run through **Valve's ARM64 Proton** +
**FEX** (x86→ARM64) + DXVK/VKD3D on Vulkan.

Layers, top to bottom:

| Layer | Where |
|---|---|
| Compose UI / activities / services (Kotlin) + managers (Java) | `app/src/main/java/com/droiddeck/launcher/` |
| Native: Wayland compositor, frame generation, fake input, Vulkan present | `app/src/main/cpp/` (CMake, NDK) |
| AdrenoTools: rootless GPU driver loading for the compositor | `app/src/main/cpp/adrenotools/` (vendored) |
| Bundled bionic GPU drivers (Adreno Turnip) | `app/src/main/assets/graphics_driver/*.tzst` |
| Linux runtime rootfs (gamescope, Mesa/Zink, glibc Turnip, proot, Proton) | downloaded at runtime from a catalog |
| glibc preload shims + guest scripts + build tooling | `tools/` |

GPL-3.0. Runtime/input/audio work is ported from WinNative, Bannerlator and Winlator.
Packaging: `compileSdk 34`, `minSdk 26`, `targetSdk 28` (deliberate — an app on 29+ may not `exec`
a file out of its own data directory, and exec'ing a rootfs is the whole job), `arm64-v8a` only,
signed with the **AOSP testkey** in `keystore/testkey.p12` (password `android`).

**The current graphics assumption, in one sentence:** every session ends in a **Turnip** (Mesa
Adreno) driver — a bionic one loaded into the app process through AdrenoTools, and a glibc one
inside the runtime — because the *reason* Turnip is required is a Vulkan extension the system
Adreno driver lacks, and because the runtime's compositing needs KGSL.

---

## 2. How it builds and runs today

### Build

* **Canonical local build:** `tools/build_local.sh`.
  Requires Docker (cross-compiles the glibc ARM64 preload libraries in `debian:bullseye`),
  **Java 17**, Android SDK + NDK, `zstd`, `curl`, `tar`, `unzip`, `shasum`, and `gh` if
  `tools/*/release.env` pins Gamescope/wlroots/esync assets. It stages
  `stageSessionScripts` (`tools/linuxfs/overlay` → `app/src/main/assets/linuxfs`), replaces
  `pulseaudio.tzst`, and produces `app/build/outputs/apk/release/app-release.apk`.
* **CI:** `.github/workflows/build.yml` — `ubuntu-24.04`, temurin **17**, the runner's NDK passed
  as `-PndkVersion=…`, `./gradlew assembleRelease`, then `./gradlew testReleaseUnitTest`.
* **Debug + device:** documented in `docs/agent-control.md`:
  `./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`;
  `tools/deploy_local.sh` installs it; `tools/droiddeckctl` (debug APKs only, `DUMP`-protected
  provider) drives sessions over ADB (`state`, `start steam`, `wait ready`, `run`, `stop`, `logs`,
  `screenshot`).

### Run

1. Install the APK (testkey-signed, so it installs over any prior build).
2. Install the **Linux runtime** from the in-app catalog (opt-in; ~3 GB) — this is a downloaded
   rootfs, not built from this repo.
3. Turn off Android's *Restrict child processes* (Developer options) or the session is killed.
4. **Play** → Steam downloads on first launch; or **Desktop & apps** → LXQt/labwc desktop.

### What I verified on this machine (Windows 11, Git Bash)

Verified:

* Gradle wrapper **8.10.2** resolves and runs; project configures; **`configuration-cache` + parallel
  build work**.
* `./gradlew :app:testReleaseUnitTest -PndkVersion=28.2.13676358` with
  `JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"` (JBR 21) and
  `ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk`:
  **315 tests, 274 passed, 41 failed.** The failure distribution is
  `AssertionError` ×13, `IOException` ×11, `FileSystemException` ×8, `NoSuchElementException` ×6,
  `UnsupportedOperationException` ×5, `NoSuchFileException` ×2, `NotLinkException` ×1,
  `ComparisonFailure` ×1, `JSONException` ×1 — all in tests that build fake directory trees with
  symlinks (`ArchivePathsTest`, `RuntimeRemovalTest`, `SecondaryLibraryTest`, `LegacyNamesTest`,
  `SessionLogShareTest`, `GameFilesTest`, `AddedGames*`, `EsyncPacksTest`, …).
  This host cannot create symlinks at all —
  `cmd //c mklink` → *"You do not have sufficient privilege to perform this operation"* — which
  explains the file-system exception classes directly and the `AssertionError` /
  `NoSuchElementException` ones as their fallout (fixtures that never got built). **All 22 tests in
  the `gpu` package pass** (`GpuModelFallbackTest`, `DriverPairsTest`, `SocNamesTest`,
  `DriverBundleTest`, `FrameGenTest`, `LosslessTest`).
  Cause: environment, not code — the working tree is clean, no file was modified, and every failing
  test is one this task does not touch. I did **not** bisect each of the 41 individually (that needs
  a Linux host or Windows Developer Mode).
* `./gradlew :app:assembleDebug -PndkVersion=28.2.13676358` — **BUILD SUCCESSFUL** in 1m 27s.
  Result: `app/build/outputs/apk/debug/app-debug.apk`, 35 294 946 bytes,
  sha256 `96becc8cd4e7ec133b0e39ef687df024ba0ae7cff7e0d73403dc39778d7cda44`.
  This exercises the whole real chain on this host:
  NDK **28.2.13676358** (overriding the `27.3.13750724` fallback) + CMake 3.22.1 compile the C/C++
  targets (`libdroiddeckwayland.so`, `libdeviceinfo.so`, `libsdnative.so`, the adrenotools hook
  libraries) into `app/.cxx/Debug/*/arm64-v8a`, `stageSessionScripts` stages the `linuxfs` overlay
  into `assets/`, and the APK carries both Adreno driver bundles plus the PulseAudio/session assets.
  The only warnings are deprecation/`always true` notices and an AGP note about
  `android:extractNativeLibs`; nothing failed.

Reproduced from a CLI-only environment, **`assembleDebug` is therefore a verified local dev loop**
(get the JDK from Android Studio's bundled JBR 21 — Gradle 8.10.2 does not support the JDK 24/25 on
this box):

```sh
export JAVA_HOME="/c/Program Files/Android/Android Studio/jbr"   # JDK 21
export ANDROID_HOME="$LOCALAPPDATA/Android/Sdk"
./gradlew :app:assembleDebug -PndkVersion=28.2.13676358
```

**Not verifiable here** (blockers, not passes):

* `tools/build_local.sh` cannot run: **no Docker, no `zstd`, no `gh`** on this machine, and it
  wants Java 17 (only JBR 21, JDK 24/25 and JRE 8 are installed). So the release artifact shape
  (glibc preload shims inside the APK) is **not** reproduced locally.
* The runtime rootfs and the catalogs are remote artifacts; nothing about the guest side can be
  exercised without a device.
* No Android device is attached (`platform-tools/adb` exists in the SDK, no transports). Neither
  `tools/deploy_local.sh` nor `tools/droiddeckctl` can be run, so **no session has been started**.

---

## 3. Where device / chipset capability handling lives

### 3.1 The single source of truth: `GpuInfo.kt`

`app/src/main/java/com/droiddeck/launcher/gpu/GpuInfo.kt` — **the single owner of what GPU this is**,
and the only file pass 2 put new device knowledge in.

* `Family` now carries Arm's generations beside Qualcomm's — `MALI_G715` ("Mali-G715"),
  `MALI_G78`, `MALI_UNKNOWN` ("Mali") — and `Family.vendor` maps every family to a `Vendor`:
  `ADRENO`, `MALI` or `OTHER`. `NOT_ADRENO` now means only "a GPU of neither vendor".
* Detection is two questions, each decidable without a device because each takes its paths as
  parameters (pass 3):
  * `isAdreno(kgslDir, vulkanDriver)` — the KGSL sysfs directory, or `/vendor/lib64/hw/vulkan.adreno.so`.
    Its defaults are the real paths; `@JvmOverloads` keeps the no-argument form that the driver pick calls.
  * `isMali(kbaseDevice, kbaseClass, vulkanDriver)` — **new in pass 2**: `/dev/mali0`,
    `/sys/class/misc/mali0`, or `/vendor/lib64/hw/vulkan.mali.so`. Arm's kbase stack, the opposite of KGSL.
  * `vendorOf(adreno, mali)` — Adreno wins a phone that reports both. It has no probe of its own; it
    is the one place that precedence lives.
* `detect()` takes the vendor from those, and for a Mali reads its generation with
  `maliFamilyOf(gpuName, socModel, hardware, platform)` against `MALI_GENERATIONS` plus the Tensor
  platform table (below): the GPU's own Vulkan name first, then the model/platform code names whose
  Mali is a matter of record (`gs101`/`gs201` → G78, `zuma`/`zumapro` → G715). **Nothing is inferred
  from the model number's digits** — a G78 predates a G715, but a G310 is newer than both — and a Mali
  it cannot place stays `MALI_UNKNOWN` rather than being given a generation (and so a driver) it may
  not be.
* A Mali is named from its family, never from `Build.HARDWARE`, which on a Pixel is a platform code
  name ("zumapro") that says nothing to a person.
* `support` is unchanged in structure: `NOT_ADRENO` ⇒ `UNSUPPORTED`, so the Mali families fall through
  to **`UNTESTED` — experimental hardware, not a refusal**.
* **The Mali line, one owner (pass 3):** `supportText(context)` is the card's line in the app's
  language, and a Mali's text is `gpuinfo_mali_experimental` (*"Experimental: Mali support is new and
  untested"*). `supportTextInEnglish(context)` is the device report's line: a Mali's is the same
  resource read in English, so the words live only in `strings.xml`. The Adreno lines keep their
  English literals there, because those reports have always carried them — see §8.
* **The Tensor codename table, one owner (pass 3):** `GpuInfo.TENSOR_PLATFORMS` maps `gs101`, `gs201`,
  `zuma`, `zumapro` to a chip name and a Mali family. `SocNames.name()` reads the chip name from it
  and `maliFamilyOf` reads the family from it. Tensor G5 is absent on purpose: it is an Imagination
  PowerVR, not a Mali.
* `SocNames.kt` (same package) names the chip: "Tensor G4 (GS401)" rather than a family name.

**Assumptions encoded (still):** Adreno⇒KGSL; a GPU family's driver build is chosen by generation;
Mali hardware is *recognised* but has none of what a session needs — no bundled driver, no guest-side
ICD, and the plumbing in §3.4-§3.5 unchanged. That gap is items 5 and 10 in §4.

### 3.2 The gate: `MainActivity.kt` + `GpuInfo`

* **`DeviceSupport` was removed in pass 3.** It was a one-line pass-through to `GpuInfo.isAdreno()`;
  its two callers in `MainActivity` now call `GpuInfo.isAdreno()` directly.
* **A Mali still gets the warn-only dialog, by design.** `isAdreno()` is false for it, so
  `MainActivity.onRuntimeButton` and `MainActivity.startSession` still warn before the download.
  Pass 2 changes what the app *knows* about the device, not what a session can promise on it — items
  5 and 10 are what let that relax.
* The **only hard gate** is in `MainActivity`'s `onRuntimeButton`/`startSession`: with no runtime
  installed, a non-Adreno device gets a `ConfirmDialog` (strings `main_non_adreno_title` /
  `main_non_adreno_text`, confirm button `main_install_anyway`). It **warns and lets the user
  continue**; it does not block.
* `ui/FrontEndSetup.kt` — Setup's "System check" row: `TESTED` green, `UNTESTED` warn (amber, with
  `setup_gpu_untested_detail` filled from `supportText(ctx)` — which is how a Mali now reads
  "… experimental: Mali support is new and untested"), everything else "GPU is not supported" +
  `setup_gpu_unsupported_detail` (*"%1$s is not an Adreno GPU; DroidDeck's Turnip driver needs the Adreno KGSL device, so Steam has no GPU device to draw on"*),
  which a Mali never produces (it is `UNTESTED`, not `UNSUPPORTED`).
* `DriverMenus` → `ui/GpuDriversPanel.kt` — the GPU drivers panel: `unsupported = gpu.support ==
  UNSUPPORTED` hides the driver-pair list entirely and paints the card red. A Mali is `UNTESTED`, so
  it is amber and its panel *is* shown — with an empty pair list, because `recommendedKey` gives it no
  pair (§3.5, §4 item 3).

### 3.3 Which driver a session uses — two different drivers

**(a) Display driver (bionic, in the app process)** —
`gpu/TurnipDriver.java`, `wayland/WaylandCompositor.java`, `wayland/CompositorHost.kt`,
`SessionActivity.kt`, `cpp/waylandcomp/src/vk_loader.c`.

* Two Adreno Turnips ship in the APK: `turnip25.1.0` (Adreno 7xx) and `turnip-sdk36` (Adreno 8xx)
  (`TurnipDriver.BUNDLED`), unpacked from `assets/graphics_driver/*.tzst`.
* `chooseByDevice()` reads `/sys/class/kgsl/…` and — when it finds **nothing** — used to *still
  return an Adreno Turnip* (SDK ≥ 36 → `DRIVER_A8XX`). On a Tensor phone "Auto" therefore selected
  an Adreno Turnip and handed it to AdrenoTools. **Fixed in pass 2:** the decision is now the pure
  `static String bundledDriverFor(GpuInfo gpu, String requested, int sdk)`, which answers for the
  device's stack: a Turnip by generation for an Adreno, `DRIVER_MALI_G715` for a Mali-G715, `null` for
  a Mali it cannot place and for a GPU of neither vendor. So nothing that cannot load is unpacked and
  the compositor falls back to the system Vulkan loader. The Adreno generation pick and its SDK
  fallback are exactly what they were, and `TurnipDriverPickTest` asserts both (§9).
* **Only what the apk carries is ever offered (pass 4):** `carried(id)` checks
  `assets/graphics_driver/adrenotools-<id>.tzst`, and `visibleBundled()`, the Auto pick and the pinned
  path all go through it. The Mali id is in `BUNDLED` with no archive behind it yet, so it is listed
  nowhere, the log names the missing archive when the GPU would need it (§10), and dropping the
  archive in is all it takes to arm it.
* **Pinned drivers (pass 3-4):** `choose()` honours a pinned bundled build only through
  `bundledDriverFor`, and it says so first: `TurnipDriver.refusesPinned(gpu, pinned)` is true when the
  pin names a build for the *other* stack — an Adreno Turnip on a Mali or an Xclipse, the Mali build
  on an Adreno — `choose()` logs it and returns `null`, and `DriverMenus` shows the reason
  (`drivers_pinned_not_adreno`, `drivers_pinned_not_mali`) in the existing status line.
* `vk_loader.c` opens it with `adrenotools_open_libvulkan(..., ADRENOTOOLS_DRIVER_CUSTOM, …)` and
  falls back to the **system `libvulkan.so`** if that fails, logging
  *"dmabuf import likely unsupported"*. The class doc of `TurnipDriver` states why this must be
  Turnip: the system Adreno driver does not implement `VK_EXT_image_drm_format_modifier`, so
  importing the dma-bufs gamescope hands over fails and the session renders nothing.
* AdrenoTools itself is an **Adreno/KGSL-specific loader** (`adrenotools/src/driver.cpp` issues
  KGSL ioctls; `src/hook/kgsl.h`).
* Note: the **import path is vendor-agnostic in effect** — a zip is accepted when it has a
  `meta.json`, a `libraryName`, an AArch64 ELF `.so` that does not link `libc.so.6`
  (`TurnipDriver.rejectionReason()`). A Mali/PanVK-style AdrenoTools zip passes validation today and
  is importable; the *Auto* pick names the Mali build since pass 4, and `carried()` filters it out
  while the apk has no archive for it (§10).

**(b) Game driver (glibc, inside the runtime)** —
`gpu/LinuxVulkanDriver.java`, `gpu/LinuxVulkanDriverManager.java`, `runtime/LinuxRuntime.java`
(`vulkanIcd()` prefers a manifest whose name contains **`freedreno`**), `session/SessionService.kt`.

* Default `""` = the runtime's own `usr/lib/libvulkan_freedreno.so` (a glibc Turnip with the KGSL
  backend). Otherwise an imported `-Linux` zip is handed to the guest as `BL_VK_DRIVER`; the guest
  script `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` validates manifest +
  library and exports `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`.
* `SessionService.tuDebug()` and the client environment add **Turnip-specific**
  switches (`TU_DEBUG`, `MESA_LOADER_DRIVER_OVERRIDE=zink`, `ZINK_DESCRIPTORS=lazy`).

### 3.4 Device nodes and sysfs the session is handed

`runtime/LinuxRuntime.java`

* `KGSL_DEVICE` (now `GpuInfo.KGSL_NODE`, pass 3) is the node; `binds()` binds `/dev`, `/proc`, `/sys`,
  the shm dir, the app dirs, storage and the session root.
* `bindGpuNode()` — because an app may not open `/dev/dri` (nodes are labelled `graphics_device`), it
  **fabricates a render node for KGSL**: `cache/drm/dri/renderD<minor>`, `dev` and `uevent` files
  (`DRIVER=` `GpuInfo.KGSL_NAME`), a `driver` symlink to `/sys/bus/platform/drivers/msm_drm`, then binds
  it at `/dev/dri` with `/dev/kgsl-3d0` standing in as the node itself.
* `bindAdrenoStats()` + `gpuTempSource()` bind KGSL's sysfs (`GpuInfo.KGSL_DIR`) to the paths
  Valve's **mangoapp** reads (`/sys/kernel/debug/dri/0/perf_now`,
  `/sys/devices/platform/soc@0/3d00000.gpu/devfreq/3d00000.gpu/cur_freq`, thermal zones).
* `runtime/BwrapSpawner.kt` (`gpu()`) — for Flatpak sandboxes, returns `null` unless
  `GpuInfo.KGSL_NODE` exists **and** the runtime has `usr/lib/libvulkan_freedreno.so`;
  writes `freedreno_icd.json`, sets `VK_DRIVER_FILES` + Zink. On Mali this path silently degrades
  to CPU (llvmpipe).

### 3.5 Everything else that branches on the GPU vendor

| Site | What it assumes |
|---|---|
| `session/ComponentsManager.kt` (`FORCED_PACKAGES`) | DXVK 2.7.1 for `Family.A6XX`, keyed by Adreno family |
| `session/GpuClockPin.kt` | clock pinning = KGSL power-control on `GpuInfo.KGSL_NODE` |
| `session/GpuMemComponent.kt`, `GpuStatsComponent.kt` | HUD VRAM/load read from KGSL sysfs (`/sys/class/kgsl/kgsl/page_alloc`, `proc/<pid>/gpumem_*`) |
| `gpu/DeviceInfo.kt` (`gpuRange()`) | GPU clock range — **already has a Mali fallback** via `/sys/class/devfreq/*mali*`; the KGSL files above it do not |
| `gpu/DriverPairs.kt` (`recommendedKey`) | pairs and the Auto recommendation are per Adreno family; `recommendedKey` returns `null` for `NOT_ADRENO` **and for the three Mali families** (pass 2), so Auto sets no pair on a Mali and the panel's list stays empty rather than offering a Turnip that could not load |
| `core/DeviceReport.kt` | session report records KGSL `gpu_model`/`chipid` and only the `vulkan.adreno.so` path |
| `gpu/SocNames.kt` | the SoC name table is Snapdragon-only, plus the Tensor platforms read from `GpuInfo` |
| `tools/linuxfs/preload/*.c` | `kgslid.c`, `kgslpoll.c`, `drm.c` (KGSL GEM_CLOSE), `tracefs.c` — KGSL interceptors; they no-op elsewhere |
| `tools/linuxfs/overlay/.../droiddeck-session`, `droiddeck-flatpak-setup` | comments/behaviour tied to the KGSL Turnip backend |
| `app/src/main/res/values*/strings.xml` (9 locales) | `main_non_adreno_*`, `setup_gpu_unsupported*`, `gpuinfo_unsupported`, `perf_gpuclock_hint`, `perf_sysmem_hint` |
| `README.md` | *"Mali, Xclipse, PowerVR, and Adreno 710 are unsupported"* |

---

## 4. What "enable the usage of Google Tensor chips" means here

Google Tensor is **Mali** (Tensor G1/G2/G3 = Mali-G78 MP20, G4/G5 = Mali-G715), i.e. **Valhall with
Arm's proprietary `kbase` kernel driver** (`/dev/mali0`) — the exact opposite of KGSL. So this is
**not** "add a chip to a table": the whole graphics stack the app leans on is Turnip-over-KGSL.

Two facts decide how far the change goes:

1. **The app-side half is tractable.** Mesa's open-source **PanVK** now covers Valhall (V10/V11),
   and an AdrenoTools-packaged PanVK-on-kbase driver already exists in the wild
   (e.g. `FristOneRR-Panvk-Driver`, distributed exactly like a Banners-Turnip zip, built for
   Winlator/Bannerlator — the same AdrenoTools lineage DroidDeck uses). DroidDeck's importer would
   accept such a zip today; nothing else in the app would offer it.
2. **The guest-side half is the real project.** The Linux runtime draws with a **glibc** driver. A
   Mali blob driver is bionic and cannot be loaded by a glibc guest; the guest therefore needs a
   **glibc PanVK build** for kbase (or the runtime must be rebuilt with one), plus the runtime's
   `vulkanIcd()`/`freedreno` assumptions relaxed. And D3D12 is off the table: PanVK-on-kbase is
   beta and non-conformant, DXVK 2.x needs a wrapper, and BCn/ASTC behaviour differs from Adreno.

### The concrete edits, per site

| # | File(s) | Edit |
|---|---|---|
| 1 | `gpu/GpuInfo.kt` | **DONE (pass 2, tidied in pass 3)** — vendor notion (`ADRENO`/`MALI`/`OTHER`), kbase detection, `Family.MALI_G78/MALI_G715/MALI_UNKNOWN` + `MALI_GENERATIONS`, `support` to `UNTESTED` with Mali wording, naming from the family instead of `Build.HARDWARE`, all in `GpuInfo` alone (§3.1, §6, §8) |
| 2 | `core/DeviceSupport.kt` (removed), `MainActivity.kt`, `ui/FrontEndSetup.kt` | **Part done:** the wrapper is gone; callers use `GpuInfo.isAdreno()`. Setup's row already renders a Mali as untested (amber + the Mali wording). **Not done:** the non-Adreno *dialog* still says the app needs an Adreno, and its strings are unchanged in all 9 locales |
| 3 | `ui/GpuDriversPanel.kt`, `DriverMenus.kt`, `gpu/DriverPairs.kt` | **Part done:** `recommendedKey` returns `null` for the Mali families, so Auto sets no Adreno pair (it reports `drivers_none_to_set`) and the pair list is empty on a Mali; the panel is not hidden because a Mali is `UNTESTED`, not `UNSUPPORTED`. **Not done:** giving a Mali its own driver list — that is item 5's artifact |
| 4 | `gpu/TurnipDriver.java` (`bundledDriverFor`) | **DONE (pass 2).** The Adreno-Turnip-on-a-non-Adreno bug is closed: `bundledDriverFor` returns `null` when the GPU is not an Adreno, so nothing is unpacked and the compositor falls back to system Vulkan (§3.3a). Pass 3 made the pinned case explicit (§3.3a) |
| 5 | `gpu/TurnipDriver.java` (list/import/pick), `ui/GpuDriversPanel.kt` | **DONE except the archive (pass 4).** The pick, the list, the delete guard and the pinned/override paths are vendor-aware; the id is `panvk-g715` and the archive would be `assets/graphics_driver/adrenotools-panvk-g715.tzst`. That archive cannot be built or fetched in this environment (§10), so `carried()` keeps the un-backed id out of every list and Auto falls back to the system Vulkan driver |
| 6 | `cpp/waylandcomp/src/vk_loader.c`, `vk_present.c`, `ahb_swapchain.c` | The system-Vulkan fallback exists, but presenting assumes dma-buf import (`VK_EXT_image_drm_format_modifier`). Needs a capability check and a shared-memory / non-modifier present path, or a clear refusal instead of a black screen |
| 7 | `runtime/LinuxRuntime.java` (`bindGpuNode`) | Fabricate the DRM node for `/dev/mali0` instead of (as well as) KGSL's: correct `uevent` (`DRIVER=mali`/`panthor`), `driver` symlink, and the node the driver actually opens (`/dev/mali0` is `graphics_device`-labelled too — same SELinux problem `bindGpuNode` exists to solve) |
| 8 | `runtime/LinuxRuntime.java` (`vulkanIcd`) + `gpu/LinuxVulkanDriver.java` + `LinuxVulkanDriverManager.java` | **DONE (pass 4).** The ICD discovery picks the manifest of the device's own stack (`runtimeVulkanVendor`), the importer takes any glibc `libvulkan_*.so` and keeps it under its own name with the manifest pointing at that name, `libraryName()` still resolves an install from before the rename, and the refusal wording names the general rule |
| 9 | `runtime/BwrapSpawner.kt` (`gpu()`) | **DONE (pass 4).** The node (`/dev/kgsl-3d0` or `/dev/mali0`), the runtime library (`libvulkan_freedreno.so` or `libvulkan_panvk.so`) and the ICD file written for the sandbox all come from the device's stack through `LinuxRuntime.runtimeVulkanVendor/Library`; the Adreno plan is byte-for-byte the plan it made before |
| 10 | Guest driver artifact | **Out of app scope, and the critical path:** a glibc PanVK-for-kbase build inside the runtime (or an importable `-Linux` PanVK zip) — this is what makes a game actually render |
| 11 | `session/GpuClockPin.kt`, `GpuMemComponent.kt`, `GpuStatsComponent.kt`, `DeviceInfo.kt`, `DeviceReport.kt` | Mali equivalents for the HUD (devfreq/mali sysfs) or explicit "not available on this GPU" degradation; `DeviceInfo.gpuRange()` already does this and is the model to copy |
| 12 | `session/ComponentsManager.kt` (`FORCED_PACKAGES`), `SessionService.kt` (`tuDebug`, `addClientEnvironment`) | Per-vendor Proton/DXVK forced packages and env (no `TU_DEBUG`, no forced Adreno sysmem rendering; likely a DXVK-version rule for PanVK's feature level) |
| 13 | `README.md`, `strings.xml` ×9, `docs/` | Requirements text and the support wording |

Effort shape: items 1-5 + 7 + 13 are ordinary app changes (a day or two of work, testable with unit
tests). Items 6, 8, 9, 11, 12 are plumbing (a few days each, need a device). Item 10 is a
**driver-engineering project** — the actual blocker for "Steam runs on a Pixel".

Pass 2 took item 1 and item 4 whole, and the decision halves of items 2 and 3; §6 records it, §7
says where items 5 and 10 attach, and §8 records pass 3.

---

## 5. Decisions taken, and what is still open

Taken (this pass):

1. **Scope — full support, bundled driver included.** Items 1-13 below are all in scope, up to and
   including a Mali driver the app ships or curates (item 5) and a glibc PanVK for the guest
   (item 10), plus HUD/telemetry, docs and validation. Plan accordingly: items 1-5 + 7 + 13 are
   ordinary app work; 6, 8, 9, 11, 12 are plumbing that needs a device; **item 10 is the critical
   path** and is driver engineering, not app work.
2. **Target — Tensor G4/G5 (Pixel 8/9/10), Mali-G715.** Detection and family naming should still be
   written generation-agnostically, but G715 is what gets validated and what the bundled driver must
   serve.
3. **Hardware — a Tensor device exists but is withheld until testing is needed.** So: build and
   unit-test first, and do not plan on device-side verification until it is handed over. Nothing in
   §4 items 6, 8-12 can be *verified* before then.
4. **Remotes — the conventional fork layout.** `origin` =
   `https://github.com/Fizzolas/DroidDeckTensor`, `upstream` = `https://github.com/Droid-Deck/DroidDeck`.
   (Both fetched and at `9310d19`. The branch is not yet pushed, so it has no upstream tracking yet.)

Still open — needed before item 5 and item 10 can be written. Pass 2 changed nothing here, and was
written so that neither answer is baked in (a Mali is `UNTESTED` **with no driver**, rather than
"supported" with one, which is the honest state until both are answered):

* **Where the Mali driver comes from.** A third-party beta PanVK-on-kbase build (fast to integrate,
  non-conformant, GPL-3.0 attribution and redistribution questions) versus building Mesa PanVK
  ourselves for G715/kbase (slower, clean licence story, we control the patches). This choice decides
  what `assets/graphics_driver/adrenotools-panvk-*.tzst` would contain and whether the guest's
  `-Linux` half is ours to build.
* **What "supported" is allowed to promise** on Mali: which DirectX versions are in the acceptance
  criteria (D3D9/D3D11 via DXVK work; **D3D12/vkd3d-proton does not**), and whether Steam's client UI
  (Chromium → ANGLE → Zink → the driver) is expected to run — that is the first thing that will
  break and it is not a game.

---

## 6. Pass 2: what changed, and what was verified

### The change

| File | Change |
|---|---|
| `gpu/GpuInfo.kt` | Vendor notion (`Vendor.ADRENO/MALI/OTHER`, `Family.vendor`); `isAdreno()`/`isMali()`/`vendorOf()`; `MALI_GENERATIONS` + `maliFamilyOf()`; `MALI_G715`/`MALI_G78`/`MALI_UNKNOWN` families; Mali `supportText`; a Mali named from its family |
| `gpu/SocNames.kt` | `TENSOR_PLATFORMS` (moved to `GpuInfo` in pass 3): `gs101`→Tensor G1, `gs201`→Tensor G2, `zuma`→Tensor G3, `zumapro`→Tensor G4 |
| `gpu/DriverPairs.kt` | `recommendedKey` returns `null` for the Mali families — every pair it lists is a Turnip |
| `core/DeviceSupport.kt` | `adreno()` delegated to `GpuInfo.isAdreno()` (file removed in pass 3) |
| `gpu/TurnipDriver.java` | New `bundledDriverFor(...)`; `chooseByDevice()` logs and returns `null` on a non-Adreno instead of picking an Adreno Turnip |
| `res/values/strings.xml` | `gpuinfo_mali_experimental` (English only so far — the other 8 locales fall back to it) |

The Adreno path's behaviour is intended to be **exactly** what it was: every Adreno family, support
level, wording and driver pick is asserted unchanged in the tests.

### Tests (as of pass 2)

New: `gpu/GpuVendorTest.kt` (detection, vendor mapping, Mali labels, `maliFamilyOf` precedence and
case, Mali = experimental not refused, the Adreno answers unchanged), `gpu/TurnipDriverPickTest.kt`
(null on a non-Adreno, the Adreno generation picks and SDK fallback unchanged),
`gpu/GpuSupportTextTest.kt` (Robolectric: the Mali *string resource*, and that a Mali family names
itself in every language). Extended: `DriverPairsTest.aMaliIsOfferedNoAdrenoPair`,
`SocNamesTest.tensorsAreNamedAsChipsNotFamilies`.

The pass 2 record: `./gradlew :app:testReleaseUnitTest --tests "com.droiddeck.launcher.gpu.*"` →
9 classes, 37 tests, 0 failures; `assembleDebug` → 35 298 591 bytes, sha256
`5c295f69a6e93ddbfa2545c7ca1738d1077b4b099389ee7f71c930b415d61f17`.

**Not verified, and not claimed:** everything that needs a device. `detect()`'s own file reads, the
dialogs, the driver panel and any session are untested here — the Mali path is proven at the
decision level (pure functions + resources), which is what §5.3 allows until the Tensor phone is
handed over. The Adreno path is likewise verified as *unchanged in its decisions*, not re-run on
device.

---

## 7. Where the next slice hooks in

The next slice is the driver itself: **item 5** (a Mali display driver the app ships) and **item 10**
(the guest's glibc PanVK — the critical path). Everything it must touch is below; nothing else in the
app needs to move for it, because the knowledge of *what GPU this is* stays in `GpuInfo` alone.

**Pass 4 took all of the wiring below (items 1-6) and item 8 with it — §9 records it.** What is left of
item 5 is one thing: the archive (§10). The seam is settled; do not reopen it.

### 7.1 Item 5 — the bundled Mali display driver (wiring done in pass 4, archive outstanding)

1. **The artifact.** `app/src/main/assets/graphics_driver/adrenotools-<id>.tzst`, beside
   `adrenotools-turnip25.1.0.tzst` and `adrenotools-turnip-sdk36.tzst`. The name is a contract:
   `TurnipDriver.install()` unpacks `graphics_driver/adrenotools-<id>.tzst` for the id it picked, so
   the id and the asset name must agree. A PanVK-on-kbase zip needs no importer change — it is
   AArch64, bionic, and does not link `libc.so.6`, which is everything `rejectionReason()` checks.
2. **The pick is settled** — `TurnipDriver.bundledDriverFor(GpuInfo, String requested, int sdk)`, one
   function for both stacks, with `buildVendor(id)` the single vendor test behind it and
   `refusesPinned(gpu, pinned)` using the same one. `TurnipDriverPickTest` pins the contract (pass 4
   rewrote it deliberately). Another Mali generation is a `switch` arm here plus a new id, nothing
   else.
3. **`TurnipDriver.BUNDLED` holds the Mali id** and the list/delete/restore path works for it —
   `visibleBundled()` additionally filters by `carried()`, so a build whose archive is not in the apk
   is invisible rather than offered.
4. **Keep the owner.** Whether this device is a Mali, and which Mali, must keep coming from `GpuInfo`
   (`Vendor`/`Family`) and not from a second set of path checks inside `TurnipDriver` — the same rule
   pass 2 applied to the old `DeviceSupport`.
5. **Words.** Done: `HELP_TEXT` and the class doc name both stacks, and the pinned refusal names its
   direction (`drivers_pinned_not_mali`, English-only — 8 locales fall back, alongside
   `gpuinfo_mali_experimental`, which is still owed its translations).
6. **Auto and the panel.** Unchanged by design: `recommendedKey` still returns `null` (a bundled Mali
   driver is not a *pair*), `DriverMenus.ensureAuto` reports `drivers_none_to_set` with the Mali
   wording, and `refreshDrivers()` offers the Mali build in the display list with no UI change once
   the archive is there.

### 7.2 Item 10 — the guest's glibc PanVK

1. `LinuxRuntime.vulkanIcd()` — **done in pass 4:** it picks the manifest of the device's own stack
   (`runtimeVulkanVendor(mali)`), with the vendor as a parameter so `LinuxRuntimeIcdTest` can decide
   it without a device. `SessionService.addClientEnvironment` is still its only caller setting
   `VK_ICD_FILENAMES`.
2. `LinuxVulkanDriver.ENV` = `BL_VK_DRIVER` (`LinuxVulkanDriver.java`), read by the guest script
   `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`, which exports `VK_DRIVER_FILES` /
   `VK_ICD_FILENAMES`. That block is driver-agnostic (manifest + readable library), so a PanVK ICD
   works through it unchanged.
3. `LinuxVulkanDriverManager` — **done in pass 4:** the zip scan takes any glibc `libvulkan_*.so`, the
   library keeps the name its zip gave it, the generated manifest points at that name, and
   `libraryName(id)` still resolves an install from before the rename (meta, then `LIB_NAME`, then any
   `libvulkan_*.so`). `lvd_no_library`'s English names the general rule; the other 8 locales still
   name freedreno and are only shown when no `libvulkan_*.so` was found at all, so they stay true -
   translating them is still owed.
4. `SessionService.tuDebug()` is Turnip-only (`TU_DEBUG`), and `addClientEnvironment()` forces Zink
   (`MESA_LOADER_DRIVER_OVERRIDE=zink`) — PanVK has its own debug switches and its own answer for the
   client's GL.

### 7.3 The rest of the map, unchanged from §4

* **Device node:** `LinuxRuntime`'s `KGSL_NODE` use and `bindGpuNode()` — the fabricated render node,
  and the SELinux problem it exists for, are KGSL-shaped; a Mali needs `/dev/mali0`.
  `bindAdrenoStats()` and `gpuTempSource()` are the HUD half of the same file.
* **Flatpak:** `BwrapSpawner.gpu()` — the KGSL node check and `freedreno_icd.json`.
* **Present path (items 6):** `cpp/waylandcomp/src/vk_loader.c` (`adrenotools_open_libvulkan(...,
  ADRENOTOOLS_DRIVER_CUSTOM, …)`), `vk_present.c` (the required dmabuf-import extension list),
  `ahb_swapchain.c`.
* **HUD/telemetry (item 11):** `gpu/DeviceInfo.kt` `gpuRange()` (whose Mali devfreq fallback is the
  model to copy), `GpuStatsComponent.kt`, `GpuClockPin.kt`, `GpuMemComponent.kt`, `DeviceReport.kt`.
* **Guest components (item 12):** `ComponentsManager.FORCED_PACKAGES` and the per-vendor
  environment, plus the KGSL preload shims in `tools/linuxfs/preload/`.

---

## 8. Pass 3: consolidation

What changed, and why, so the next pass does not reopen it:

* **One owner for the Mali wording.** The card's line comes from `supportText(context)`; the device
  report's comes from `supportTextInEnglish(context)`, which reads the same resource in English for a
  Mali. The Adreno lines keep their English literals in `supportTextInEnglish`. The below-tested Adreno
  sentence differs between the card (`gpuinfo_below_tested`: "Below tested hardware (Adreno 725 and
  newer)") and the report (the literal: "Outside tested hardware (Adreno 650, 725 and newer)"). That
  is a visible wording difference today, so pass 3 kept both and did not pick one. Choosing is a
  product decision that changes text in every locale, so it is left for a deliberate change.
* **One owner for the Tensor codenames.** `GpuInfo.TENSOR_PLATFORMS` (codename → chip name and Mali
  family). `SocNames` and `maliFamilyOf` both read it.
* **Probes collapsed.** `isAdreno(kgslDir, vulkanDriver)` and `isMali(kbaseDevice, kbaseClass,
  vulkanDriver)` are each one function with the probe paths as parameters, so the unit tests give them
  real temp paths. The no-argument form that `TurnipDriver` and `GpuInfo.detect()` call is generated by
  `@JvmOverloads`. `vendorOf(adreno, mali)` was already a single function with no wrapper; it stays.
* **`DeviceSupport` removed.** Its callers in `MainActivity` call `GpuInfo.isAdreno()`.
* **Shared test fixtures.** `maliGpu`, `adrenoGpu` and `otherGpu` are defined once in `GpuVendorTest.kt`
  and used by `GpuSupportTextTest.kt`.
* **Pinned refusal (pass 3).** §3.3a describes it. Behaviour for an Adreno, and for a Mali's decisions,
  is unchanged.

Verified by the gpu unit tests run in this pass (forced rerun): 9 classes, 42 tests, 0 failures.
The clean debug assemble is recorded in the pass-3 completion report, not in this doc, so this doc does
not carry a sha256 that can go stale.

---

## 9. Pass 4: the driver seam and the guest ICD route

The critical path, in the order that gets a device furthest: which driver the app would use, then the
route that makes the guest load it. The Adreno path's behaviour and its assets are untouched.

### What changed

| File | Change |
|---|---|
| `gpu/TurnipDriver.java` | `bundledDriverFor(GpuInfo gpu, String requested, int sdk)` - one vendor-aware pick (Turnip by generation for an Adreno, `DRIVER_MALI_G715` for a Mali-G715, null for a Mali it cannot place and for neither vendor); `DRIVER_MALI_G715 = "panvk-g715"`, the id the archive is named after; `refusesPinned(gpu, pinned)` and `buildVendor(id)` refuse the other stack's build in either direction; `carried(id)` gates every offer on the apk's own archive; the Downloads override understands `panvk`/`mali`; `HELP_TEXT` and the class doc name both stacks |
| `gpu/GpuInfo.kt` | `MALI_NAME`/`MALI_NODE` public beside `KGSL_NODE` (the node a Mali driver opens); `detect()` `@JvmStatic`, so the Java pick takes the GPU as one value instead of loose flags |
| `gpu/LinuxVulkanDriverManager.java` | Any glibc `libvulkan_*.so` is imported and kept under the name its zip gave it; the generated manifest's `library_path` is that name; `libraryName(id)` resolves meta -> `LIB_NAME` -> any `libvulkan_*.so`, so an install from before the rename still works and `isInstalled` is unchanged in effect |
| `gpu/LinuxVulkanDriver.java` | `HELP_TEXT` and the class doc name both stacks and say the runtime's own driver is vendor-dependent |
| `gpu/DriverBundle.kt` | A bundle's `linux/` half is found by `libvulkan_*` and staged under its own name |
| `runtime/LinuxRuntime.java` | `vulkanIcd(context, mali)` chooses the device's own stack (`runtimeVulkanVendor`), the vendor as a parameter so it is testable; `runtimeVulkanLibrary(vendor)` is the single owner of the runtime's library name, shared with the sandbox plan |
| `runtime/BwrapSpawner.kt` | `gpu()` takes the node, the runtime library and the ICD file name from the device's stack: `/dev/kgsl-3d0` + `libvulkan_freedreno.so` + `freedreno_icd.json` on an Adreno (byte-for-byte the plan it made before), `/dev/mali0` + `libvulkan_panvk.so` + `panvk_icd.json` on a Mali |
| `DriverMenus.kt`, `res/values/strings.xml` | The pinned refusal says which direction it is (`drivers_pinned_not_mali`, English only, 8 locales fall back); `lvd_no_library`'s English names the general rule |

### What is verified, and how

* `./gradlew :app:testReleaseUnitTest --tests "com.droiddeck.launcher.gpu.*" --tests "com.droiddeck.launcher.runtime.LinuxRuntimeIcdTest"` -> **11 classes, 55 tests, 0 failures, exit 0**. New: `TurnipDriverPickTest`'s Mali cases (the Mali build for a G715 and only there; neither vendor's build reachable by pin, override or Auto; the Adreno picks, fallback and pinning unchanged), `LinuxVulkanDriverImportTest` (a PanVK zip imports and its manifest names the file that is really on disk; a Turnip zip is unchanged; a bionic or library-less zip is still refused; an install from before the rename still resolves; a half-extracted one is never offered), and `LinuxRuntimeIcdTest` (the device's own stack's manifest, the other stack's as fallback, none as null, the vendor names).
* Full suite: **348 tests, 41 failures, exit 1** - the *identical* 41 failures the pre-work log (`gradle-verify.log`: 315 tests, 41 failed) had: Windows-host artifacts (symlink creation needs a privilege the JVM does not hold, path separators), none of them in the files this pass touched. The one failure in a file that was touched, `BwrapSpawnerPlanTest.buildsTheTreeFlatpakAsksFor`, is the same symlink privilege error it always was. **No new failure, none fixed.**

### What is not verified, and cannot be here

`carried()` is exercised only through the Adreno ids: it has never been true for the Mali id, because
no archive has ever existed for it to be true about (§10). And nothing in this pass has run on a
device - no session, no compositor, no guest, no Flatpak sandbox. The wiring is proven at the decision
level, which is what "no device yet" allows.

---

## 10. The blocker: there is no Mali driver to load

Both halves of the graphics path are now wired for Mali, and each waits on one artifact that cannot be
produced in this environment:

1. **The display driver (item 5).** `assets/graphics_driver/adrenotools-panvk-g715.tzst` - Mesa's PanVK
   built for Valhall v11 on the kbase kernel driver, packaged bionic in the AdrenoTools layout
   (`meta.json` with a `libraryName`). `TurnipDriver.DRIVER_MALI_G715` is `panvk-g715`, so the file
   name is already the contract: dropping the archive in arms the pick, the driver list, the delete
   path and Auto, with no code change. It has to be *built* (Mesa plus our patches, cross-compiled for
   Android/bionic) or taken from a third-party beta build. §5 still records that choice.
2. **The guest's driver (item 10).** The runtime needs a glibc PanVK: either built into the rootfs, so
   `vulkanIcd()` finds a `panvk_icd.aarch64.json`, or imported as a `-Linux` zip, which the pass-4
   importer now accepts. Same choice, plus a glibc aarch64 Mesa the rootfs' libraries match.

**Neither has been faked.** No placeholder archive, no empty asset, no "supported" label: a Mali today
is detected, named, given no driver, told why in the log (`this apk carries no
graphics_driver/adrenotools-panvk-g715.tzst`), and left on the system Vulkan driver - where it was
before this pass. `README.md` still lists Mali as unsupported and stays that way until an archive
exists: the requirements text is the last thing to change, not the first.

---

## 11. What "working on Mali" will be judged by

The acceptance bar, written down before the driver lands so that the claim can be checked instead of
asserted. 1 and 2 are provable offline and are **done**; 3-5 need the Tensor phone (§5.3 says the
hardware is withheld until testing is needed - for a driver, that is now); 6 bounds the claim; 7 says
what a failure must look like.

| # | Criterion | Judged by | Provable offline |
|---|---|---|---|
| 1 | **Recognised**: the phone reports `Mali-G715`, chip `Tensor G4`, support `UNTESTED` (amber, not red), and no Adreno driver is picked by Auto, by a pin, by the override or by the import list | the GPU tests (§9), the device card | **yes - done** |
| 2 | **Routed**: the runtime's own ICD manifest, the runtime library the Flatpak sandbox binds and its node, and the guest's driver list all follow the device's stack; a Mali is never handed a Turnip on any of those paths | `LinuxRuntimeIcdTest`, the vendor names, the pick tests | **yes - done** |
| 3 | **The display driver loads**: the compositor gets a Vulkan device from the bundled PanVK (or the system Mali driver) and presents a frame; if dma-buf import is what fails, the log says so rather than the session going black | a session on the phone, logcat and the session log | no |
| 4 | **The guest renders**: inside the runtime Vulkan reports PanVK as the device, gamescope starts, and Steam's own client UI draws (Chromium -> ANGLE -> Zink -> the driver) | a Vulkan device dump in the session, the session log | no |
| 5 | **A game renders**: at least one D3D11 title through Proton + DXVK reaches gameplay, takes input, and has a recorded framerate | the phone, a frame counter | no |
| 6 | **Bounded**: D3D12/vkd3d-proton, HDR, the second screen and the HUD's GPU load/clock/VRAM are out of scope for the first "working" on Mali (its sysfs is not KGSL's) and must degrade to "not available", never to a wrong number | - | - |
| 7 | **Honest failure**: with no driver, a session or an install says so (log and device report) instead of starting into a black screen | logcat, the device report | partly - the log line is in place (§9) |

A build may call itself **game-ready on Mali** when 3, 4 and 5 have been run on the phone and recorded
here. Until then it is **detected and wired, not working** - which is what the README, the support
wording and every claim in this doc should keep saying.
