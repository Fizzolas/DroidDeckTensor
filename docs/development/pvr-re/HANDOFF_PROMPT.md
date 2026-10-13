# HANDOFF PROMPT — DroidDeck / Tensor G5 PowerVR: get Steam's UI running

Copy the block below into a fresh session (Claude Code, working dir `D:\GUH\DroidDeck`).

---

## PROMPT

You are picking up a long-running reverse-engineering / porting effort. **Read
`docs/development/tensor-g5-powervr-state.md` (the authoritative, sectioned record; the relevant
sections are §40–§55) and `C:\Users\sekri\.claude\projects\D--GUH\memory\*.md` first.** Do not take
screenshots (images blow the context); extract state as text (adb shell, logs, `/proc`). Build with
`JAVA_HOME=~/.jdks/jdk-17.0.20+8` (JDK 25 breaks Gradle); `assembleDebug` is signed with the public
testkey so it installs over any build; bump `versionCode` when a bundled asset changes.

### The environment

- **Device:** Google Pixel 10 Pro XL, `ro.board.platform=laguna`, `ro.hardware=mustang`,
  `ro.soc.model=Tensor G5`. adb serial `57210DLCQ002KB`. Not rooted; app runs as `untrusted_app_27`.
- **GPU:** Imagination **PowerVR D-Series DXT-48-1536 MC1** (`vendorID 0x1010`, `deviceID 0x71061212`).
  Kernel driver `pvrsrvkm` (proprietary Google/Imagination), DRM render node `/dev/dri/renderD128`
  and card node `card2` (both `226:128`/`226:2`, one device under `34f00000.gpu0`). The node has **no
  GEM and no GBM** — that is the crux of everything below. `/dev/dma_heap/*` and `/dev/udmabuf`
  exist but are **denied** to the app.
- **App:** DroidDeck fork at `D:\GUH\DroidDeck`, branch `feature/google-tensor-support`, `versionCode
  15`, `versionName 0.3.4` (`app/build.gradle`). It runs a **glibc ARM64 Linux rootfs under `proot`**
  and shows it in an Android SurfaceView. Single Gradle module; `CLAUDE.md` has the build/architecture.
- **Guest graphics stack (all inside the proot guest):**
  - **Our glibc Vulkan ICD** (`docs/development/pvr-re/hybshim/icd/pvr_icd.c` → `libvulkan_powervr.so`)
    that loads the **bionic PowerVR HAL** `/vendor/lib64/hw/vulkan.powervr.so` through **libhybris**
    (`android_dlopen_ext`). This is the only way glibc code reaches the GPU. Proven working for compute,
    raster and dma-buf export.
  - **Our WSI** (`hybshim/icd/pvr_wsi.c`): swapchain images are exportable dma-bufs, presented over
    **Wayland** (`wl_buffer`) or **X11/DRI3** (`xcb_dri3_pixmap_from_buffers`).
  - **gamescope 3.16.29** (patched; `tools/gamescope/`), nested on the app's Wayland compositor
    (`--backend wayland --expose-wayland -f -W 2404 -H 1080 -w 2404 -h 1080 -e --xwayland-count 2
    --default-touch-mode 4`). It starts 2 Xwaylands (`:0`, `:1` on sockets at
    `<files>/linuxfs/tmp/.X11-unix/X0`,`X1`). gamescope's `steamcompmgr.cpp` skips a 1×1 window
    (`win_skip_and_not_fullscreen`) and only sizes a window to the output when it is fullscreen
    (`window_is_fullscreen || force_windows_fullscreen`). It **does** honor `_NET_WM_STATE_FULLSCREEN`
    (via `handle_client_message`/`handle_net_wm_state`).
  - **Steam client** (Valve's ARM64 `steamrtarm64`), launched by `droiddeck-session` with
    `-gamepadui -steamdeck -steamos3 -no-cef-sandbox -cef-force-gpu -cef-ozone-platform=x11
    -cef-use-gl=angle -cef-use-angle=vulkan`. The UI is **CEF** (Chromium 126, `libcef.so`, 208 MB).
- **Session helper switches** (in `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`, set via
  `Download/droiddeck-env`, read with a ~1-session lag — always confirm the switch line in the new
  `session.log`): `BL_*` for the session, plus I added `BL_CEF_USE_GL/BL_CEF_ANGLE/BL_CEF_OZONE/
  BL_CEF_DISABLE_GPU/BL_CEF_EXTRA/BL_CEF_VULKAN_ICD/BL_CEF_LIBDIR/BL_UI_PROBE/BL_GAMESCOPE_ARGS`.
  Start a session from adb with
  `tools/droiddeckctl start steam` (or `run /usr/bin/<prog>`; the `SessionActivity` is not exported,
  the trampoline is `.agent.AgentStartActivity --es request <base64 {"mode":"steam"}>`).
  `HYBRIS_LINKER_DIR=<files>/linux_vulkan_drivers/powervr-g5x` is needed wherever libhybris is used.

### What WORKS (measured)

- **A guest Vulkan app renders on screen.** `tools/droiddeckctl run /usr/bin/vkcube`: selects
  `PowerVR D-Series DXT-48-1536 MC1`, renders, presents 20× `2404x1080` dma-bufs through our WSI;
  the app's compositor logs `last 10 s: 13 frames on screen | 14 GPU frames from games`. Full chain
  works: **app → glibc loader → our ICD → libhybris → bionic HAL → GPU → dma-buf → gamescope →
  DroidDeck compositor → screen.**
- **GL too:** `glxgears` renders via **mesa zink → our ICD** at `~205 FPS`.
- **Steam's UPDATE UI renders** (GLX path, `0x800002`, 2404×1080, ~21 presents).
- **The Steam client reaches its login screen** (`steamui_login.txt`: `SetLoginState:
  WaitingForCredentials`).

### UPDATE 2026-10-13d (sessions 18-20, §60) — read this FIRST, supersedes 13c's framing

**SharedJSContext is confirmed the headless controller — the user's warning was right.** Its 1×1
outer/inner/screen and blank screenshot are NOT the login page's state. CDP observer v6
(`hybshim/cdp/cdp_probe.py`, HTTP-lifecycle diff + read-only page attach; wired into the session
probe block `BL_UI_PROBE=1` in `droiddeck-session` — commit `2cfe8db`) attached to the **second**
page target (`about:blank?createflags=...&useragent=Valve Steam Gamepad`, title mutates to
**`Steam Big Picture Mode`**): **outer=1280x800, inner=1281x800, screen=2404x1080, readyState=complete,
`visibilityState=HIDDEN`**, bridge `SteamClient.{Browser,Window,BrowserView,...}`. The SharedJSContext
console shows the whole login-app JS booting fully (`SteamApp Init - Before Login` ~681 ms, transport
connected, `OnLoginUsersChanged`) inside the 1×1 controller — the controller's job (requesting the BP
window) completes; its params appear in the BP page's own title (`createflags=6292738 minwidth=853
minheight=534 browserType=4`).

**So the first-frame failure = the BP browser view is loaded but CHROMIUM-HIDDEN; Chromium does not
produce frames for hidden pages.** That explains: no swapchain on the BP surface (§57), nothing
presents, helper `_exit(0)` ~10-15 s, client `rc=0`. GBM/shim: closed chapter; session 20 ran on
**stock graphics** and still showed this — visibility, not GPU, is the wall now.

**UNRESOLVED (proven/inferred boundary — §60 caveat):** config split. With nulldev shim + old observer,
xinspect saw the native BP X window VIEWABLE (2404x1080, §56/§58) but the BP web view wasn't visible
to CDP (v2 attach races, `handshake-closed` on second page). Stock + v6: BP web view (hidden) seen,
no BP X window in xinspect rounds (may be lifecycle race: BP window lived only ~10 s in gen2's death
sequence). **One clean run resolves it: same stock setup, v6 observer (already repo-persisted),
full-session sampling; correlate xinspect's BP-window round with CDP attach of the same moment** — did
the native window exist while hidden, or was it never mapped? Then trace WHO decides visibility
(ozone-X11 window activation: `_NET_ACTIVE_WINDOW`/WM state vs gamescope never making it
focus — `focused app: none` is constant) — the fix target is CEF widget->window activation, not any
extent/resize knob. Also keep: helper `_exit(0)` after ~10 s = Steam's own shutdown path; find its
trigger if the BP-visibility answer doesn't subsume it.

### UPDATE 2026-10-13e (sessions 21-23, §61) — read this FIRST; 13d's open question is ANSWERED

**Same-run correlation DONE and PROVEN.** Two clean runs (session 22 STOCK, session 23 labelled
NULL-create, identical observer set) captured web attach + native tail in one CLOCK_MONOTONIC stream
(v7.2 `hybshim/cdp/cdp_probe7.py` spawns `xinspect2 --events` the instant the BP attach lands;
commits `057d488` `7efbc7a` `a374d0d`). Result: the BP web target (`title=Steam Big Picture Mode`,
`hidden/hidden/focus=false`, inner 1281x800 outer 1280x800, readyState=complete, kids=1
`DIV#popup_target`, innerText len=0) and its native window **coexist in the same second**:
`0x1c00033` 'Steam Big Picture Mode'/'steamwebhelper' **2404x1080 d=32 VIEWABLE WM_STATE=Normal
_NET_WM_PID=26590 child 1280x800 VIEWABLE**, gamescope set `_NET_ACTIVE_WINDOW` to it +530 ms after
attach (input focus = helper window, never entered BP). Association proven by **PID (proc_watch
pins 26590 as the CDP-owner browser) + geometry + title** — HIGH confidence, title-match caveat
satisfied. Branch A (unmapped/minimized) and E (no association) are **refuted**. §60's stock
"no BP window ever" was pure sampling lifetime: the BP window lives ~0.9 s per generation, 4 s
rounds missed it (the "one-sample absence" warning was right).

**New binding constraint (both runs):** ~850 ms after the BP window maps/activates, the GPU process
crashes: fresh PID-filtered cef_log shows 12x `shared_image_interface_proxy 'Buffer handle is null'`
+ `one_copy_raster_buffer_provider` staging failure + `gpu_process_host exit_code=11` — under STOCK
ICD (22: `[20532]` exit 11; 23: `[26590]` exit 11) **and under nulldev shim + stock ICD (23)**,
contradicting §58's "nulldev GPU silent" (that run used the instrumented ICD `b1728201` — record,
don't harmonize). Window dies with the gen → Steam orderly `_exit(0)` ~10-15 s → rc=0. Whether
activation flips hidden→visible is still UNOBSERVED: the page dies before the 6 s readback (the
window lifetime is <1 s — attach V7BP at T, T+2 s poll for a second readback, or shorten hold).
Session 21 was OBSERVER-DEFECTIVE (two fixed tooling bugs: CEF list `"id"` vs `"targetId"` key
collision; xinspect2 BadWindow default handler death) — its numbers are recorded in §61 but NOT
merged.

**Next one-variable run (STEP 6 direction, still unexecuted — no causal intervention has run):**
AHB shim `gbm_ahb.c` (the §50 artifact, cut crashes ~70→1, gives Chromium a real gralloc dma-buf)
+ stock ICD + same v7.2 observers, `cef_log` truncated fresh. Question it answers: if the GPU process
survives BP first-raster, does the BP page flip hidden→visible and produce swapchain/presents (→
branch D, compositor path), or stay hidden (→ branch B, CEF visibility propagation). Rollback:
delete `steamrtarm64/libgbm.so.1`. Device is STOCK + clean (ICD `68f90ad8…`, libgbm `e28a74e1…`,
no shim/env/observers, IDLE — sha256-verified on-device 2026-10-13 after session 23).

### UPDATE 2026-10-13c (CDP page observation + lifecycle, §59) — read this before 13b

**The login page EXISTS and is ALIVE - its viewport is 1×1.** CDP (`Chrome/126.0.6478.183`,
Protocol 1.3; endpoint `127.0.0.1` only, owner = the steamwebhelper browser; guest-local, never
forwarded) shows exactly ONE page target (`SharedJSContext` @ `steamloopback.host/index.html` —
**title/url identical for all CEF pages; the login UI lives inside that document**). At attach:
`readyState=complete`, `visibilityState=visible`, **`innerWidth×innerHeight = 1×1`**, body has 2
children; ONE bounded screenshot = 759 B, decodes to a **1×1 blank** (SOF height=1 width=1, no login pixels). One 404
resource; no JS exceptions in the window. The instrumented run then: **webhelper exits are ALL clean
`_exit(0)`** (LD_PRELOAD `_exit`/`exit`/`abort` tracer, separate session) - no crash signal; the
"Disabling sandbox due to a previous crash in CefInitialize" line is persisted config, not this run.
Browser gen1 lives ~7-10 s (CDP UP then DOWN one round later), gen2 launches, client exits `rc=0`
~14-30 s (`socket disconnected`). **Ordering: the page is 1×1 from the first moment it is visible -
before the replacement - so the gen swap is downstream, not the cause.**

**So the failure = web viewport size delivery.** X11 window: mapped, 2404x1080 (§56). The CEF page
never learns it. The client's own Vulkan swapchains (zink UI windows) are probe-then-abandon 1x1
(§57) — same "nothing sized" shape on two independent backends, pointing at **the window->contents
size signal, not the GPU stack**. Note (§57) the Big-Picture CEF window NEVER got a swapchain at all;
and the `--type=zygote`-labeled GBM callers are the GPU-process forks (constructor-inherited role
string; no LOAD line).

**NEXT STEP (narrow, one run):** attach CDP to the page (same observer set, now repo-persisted at
`hybshim/cdp/`: `cdp_watch.sh`, `cdp_page.py`, `proc_watch.sh`) and evaluate
`window.outerWidth/outerHeight`, `document.documentElement.clientWidth/Height` + a 5 s `onresize`
recorder: separates "CEF never told" (proceed to CEF surface/window-size plumbing: ozone-X11
GetWindowRect/ConfigureNotify -> views::NativeWidgetAura path, and/or gamescope configure of the
CEF window) from "told but ignored" (CEF ozone bug - escalate differently). Do NOT ship
`vk_wsi_force_swapchain_to_current_extent` or any WSI sizing change before the login-content path is
proven to touch that surface. Do not reopen GBM (matrix closed, §58). Rollback/baseline docs:
`ROLLBACK_BASELINE.md`; observer scripts in `hybshim/cdp/`; probe shim in `hybshim/gbm/`
(gbm_probe.c, GBM_SHIM_MODE=env nulldev|stub). Device stock at session end (ICD 68f90ad8).

### UPDATE 2026-10-13b (matched-control experiments, §58) — read this FIRST

Two runs, ONE byte-identical probe shim (`hybshim/gbm/gbm_probe.c`, mode via `GBM_SHIM_MODE` env),
same instrumented ICD (`b1728201`), same script (`f64e72f3`), logs at `Steam/logs/gbma.log`:

- **nulldev** (session 13-06): 2 `gbm_create_device(/dev/dri/renderD128)->NULL` (caller
  `libcef.so+0x2df97f0`, GPU processes = zygote forks with no LOAD line), then **complete GBM
  silence: 0 null handles, 0 GPU exits, 0 webhelper-restart reasons**. The Big-Picture window is
  created at 2404x1080 depth-32 and **VIEWABLE** — and **still zero login frames are ever
  presented**.
- **stub device** (13-07): device accepted; Chromium's GPU-init probe (16x 2x2 `gbm_bo_create`
  across R8/GR88/AB24/XB24/AR24/XR24/AR30/RG16/NV12/NV21/P010, callers `0x2dfa0d8`+`0x2dfa904`,
  LINEAR|RENDERING flag sets) all honestly `ENOSYS` -> **GPU process SIGSEGVs ~4 s in** (Chromium's
  init bug on all-probes-fail; `gbm_bo_import` never reached; client outcome unchanged).

**CRITICAL METHOD FACT: Steam APPENDS `cef_log.txt` across relaunches** — every earlier
null/crash count must be **filtered by the session's own PIDs** (the copied `steam/cef_log.txt`
starts with the previous run). This **retracts §57's "17 residual nulls + restart loop under
NULL-create"** (they were a 12:23 session's pid-32372 lines) and downgrades stock/AHB crash counts
to upper bounds. **The GPU/GBM/shared-image story is therefore NOT what blocks the login frame
anymore:** with GBM fully silent (nulldev), the window is mapped and sized, and still nothing
presents. The failure moved **downstream of graphics**: candidates, both cheap and single-variable —
(a) the **browser-generation restart** every ~10 s (gen1 18411 -> gen2 18724; no logged reason;
check the LIVE `webhelper.txt` for the browser's own exit line, or query CDP: these sessions run
`-cef-enable-debugging -devtools-port <p>`; the page may simply never paint because a login-UI JS/IPC
step fails); (b) CEF window->compositor (unlikely: the 64x24 overlay window from the same pipeline
presents fine through our WSI).

**Do not re-litigate GBM/shim variants — the matrix is closed** (stock: crash-heavy; AHB: probes
pass, consume-crash; nulldev: silent-but-no-frame; stub: probe-fail-crash). The one real driver-side
gap left visible in logs is our X11/DRI3 BadAlloc (`xcb_dri3_pixmap_from_buffers` on 32x32 utility
windows -> Xwayland `present: Error getting buffer`) — cosmetic for THIS objective (login window is
Wayland-path), tracked in §40-41.

### UPDATE 2026-10-13 (per-PID attribution, §57) — read this before everything below

With per-PID WSI logging (`pvr_wsi[pid]` in `pvr_wsi.c`, `BL_UI_PROBE=1` in the session script) on the
reproduced NULL-create baseline, **every attribution question from the 2026-10-12 review is answered**:
`vkCreateComputePipelines failed (-13)` is gamescope's own context (interleaves with successful
update-UI presents — **benign to the main UI**); `Error getting buffer` is an **Xwayland** message
("present: Error getting buffer", vblank buffer, downstream of our DRI3 BadAllocs); the 1x1
swapchain churn is **steam itself** (zink) probing-and-abandoning surfaces told `2404x1080`, never
acquiring/presenting them. **No process ever creates a swapchain on the Big-Picture CEF window.**
The residual blocker is CEF's browser-side shared-image loop (`~17 Buffer handle is null` +
`Restart webhelper process, counter 2` persist under NULL-create) — the crash tracks **device
creation** (matrix: stock 350null/48crash; AHB 0null/48crash; NULL-dev ~17null/0-1crash), so the
**next cheap one-variable experiment is row 4: valid stub device + failing `gbm_bo_create`** (and/or
hardening `gbm_bo_import`), i.e. separate "device missing" from "allocation missing". Device left
stock (ICD `68f90ad8`); all probes/shim removed; rollback in `ROLLBACK_BASELINE.md`.

### UPDATE 2026-10-12 (Phase 1-3) — read this before §45-55 below

The NULL-create shim (`libgbm.so.1` at `steamrtarm64/`, padded to 2046456 B) makes CEF's GPU process
**not crash** (0-1/session vs ~48 with stock libgbm): the shim loads in all 8 steamwebhelper procs,
`gbm_create_device -> NULL` is reached, `gbm_bo_create` is never called, CEF uses its non-GBM
fallback. **Blocker A is effectively resolved by that shim.**

**Blocker B is reframed — it is NOT window sizing.** With CEF not crashing, the main UI window
**"Steam Big Picture Mode" (class steamwebhelper, depth 32) reaches `2404x1080 VIEWABLE`** with
1280x800 children (live tree, session 56). **But the client's swapchain for its window
(`xid 0x1200002`) is created `1x1` (requested=1x1, via the gamescope WSI layer) and destroyed
immediately; it is never rebuilt at 2404x1080; no frame from the main client is presented (all 21
presents are the update UI); the client exits `rc=0` ~14 s in (`ProcessNextMessage: socket
disconnected`).** Our WSI correctly reports `currentExtent=2404x1080` for that surface; the gamescope
WSI layer sets the swapchain extent from the X *window rect* at creation (~1x1) via
`xcb::getWindowRect` (frog_wsi.cpp). `--force-windows-fullscreen` did not change the 1x1 this time.
The gamescope WSI layer only emits `VK_ERROR_OUT_OF_DATE_KHR` on window resize **at present time**,
which the client never reaches.

**So the next target is the client's UI swapchain/present path**: why the client builds a 1x1 probe
swapchain, never rebuilds it after the window grows to 2404x1080, presents nothing, and exits (look at
the run of `vkCreateComputePipelines failed (-13)` and `Error getting buffer` at the same time). Full
record: state doc §56. Rollback baseline: `docs/development/pvr-re/ROLLBACK_BASELINE.md`.

### What is BLOCKED (latest data, §45–§55)

**Blocker A — CEF's GPU process crashes on every buffer allocation (the graphics wall).**
CEF's GPU process dies ~48×/session. It allocates its buffers through **GBM**. The PowerVR node has no
GBM/GEM, so `libgbm.so.1`'s `gbm_bo_get_fd` fails → `Buffer handle is null`
(`shared_image_interface_proxy.cc`) → `Creation of StagingBuffer's SharedImage failed` →
`gpu_process_host.cc: GPU process exited unexpectedly` (exit_code 11=SIGSEGV, 6=SIGABRT).

- **The GBM consumer is `libcef.so`** (its `DT_NEEDED` is `libgbm.so.1`; it also references
  `gbm_create_device`, `gbm_bo_import`). NOT `steamwebhelper`, and NOT `libminigbm.so`. So a shim
  must be `libgbm.so.1` in the client's `$ORIGIN` (`steamrtarm64/`), padded to the real
  `libgbm.so.1.0.0` size (2046456 B — Steam verifies file size).
- **A working shim exists** (`docs/development/pvr-re/hybshim/gbm/gbm_ahb.c`): a `libgbm.so.1` whose
  GBM API is answered by **real gralloc buffers via libhybris** (`AHardwareBuffer_allocate` →
  real dma-buf). Installed correctly it **loads and is called**, and `Buffer handle is null` goes
  **128 → 0**. Fixed within it: `gbm_bo_map` must pass an `ARect` (not a desc) to
  `AHardwareBuffer_lock` and use CPU usage bits; `gbm_bo_import` must be implemented (not a NULL stub).
- **The remaining crash:** with the shim in place the GPU process still dies — after a run of
  `gbm_bo_create` (36×, all 2×2, format `0x20203852`="R8") and then **no further `gbm_*` call**. All 38
  `gbm_*` symbols the real libgbm exports are present, so it is not a missing symbol; the fault is in
  **CEF's own use of the buffers**. **Decisive experiment:** make `gbm_create_device` return NULL
  (CEF sees GBM unavailable) → **crash 48 → 1**: CEF has a working non-GBM fallback and runs almost
  cleanly without GBM. No in-guest handler (rig `crashcatch`, a shim constructor handler, Chromium's
  own); produces an address — the process is killed by its parent, not crashing in place; breakpad is
  off (`--disable-breakpad`).

**Blocker B — the main client UI never shows (INDEPENDENT of Blocker A).**
The client's main UI window is created (via the **gamescope WSI layer**, i.e. Vulkan) as xid
`0x1200002` with a **1×1 swapchain**, and is never mapped/shown; the client then exits `rc=0` (~30 s).
The **update UI uses GLX and DOES show** at 2404×1080. So it is specifically the client's
**Vulkan/WSI surface sizing**. gamescope won't size a 1×1 window (see above). Tried and NOT the fix:
`config.vdf` display mode Windowed→Fullscreen; gamescope `--force-windows-fullscreen`; Steam
`-fullscreen`; `-cef-use-gl=desktop` (GLX — worse, the window never appears); `BL_XWAYLAND_COUNT=1`;
`BL_STEAMDECK=0`; the env `vk_wsi_force_swapchain_to_current_extent=true` (the gamescope-WSI-layer
flag that forces the swapchain to the current extent — did not change it). `XRRGetOutputInfo() is not
available` is a red herring (the symbol exists in every loadable libXrandr; non-fatal).

Key read-only facts: `xrandr` inside the session reports `current 2404 x 1080`; the client's own
display manager logs `OnOutputMode: 2404x1080`; the live X tree shows the client's windows all
`unmapped` and gamescope `== focused app: none`.

### Variables that matter
`HYBRIS_LINKER_DIR`, `LD_PRELOAD`, `MESA_LOADER_DRIVER_OVERRIDE=zink`/`GALLIUM_DRIVER=zink` (Steam's
client renders via zink for GL), `VK_ICD_FILENAMES`/`BL_VK_DRIVER` (our ICD),
`vk_wsi_force_swapchain_to_current_extent`, `GAMESCOPE_*` (hosted runtime + patched gamescope),
Steam's own `-cef-*`/`-steamdeck`/`-steamos3`/`-gamepadui`, and the `BL_CEF_*` switches.

### What a COMPLETE project looks like
1. **CEF's GPU process runs** (Blocker A). Either (a) make the AHB-backed `libgbm.so.1` shim good
   enough for CEF to use (fix CEF's use-of-buffer crash — needs the crash address, e.g. a real
   debugger in the guest, or a Chromium build without the GBM path), or (b) ship the
   `gbm_create_device`→NULL variant so CEF uses its non-GBM fallback (crash 48→1, proven) and make
   *that* path fully work. Option (b) is the pragmatic one to pursue first.
2. **The main UI window shows** (Blocker B): make the client's Vulkan/WSI surface report/obtain the
   real 2404×1080 (gamescope focus+configure, or `ForceSwapchainExtent`, or get the client onto the
   GLX path that already renders).
3. **Package everything** into the runtime asset: the ICD (`hybshim/icd/`) + `libhybris.so` + `q.so`
   (already packaged) + the `libgbm.so.1` shim + its libhybris, wired through
   `LinuxVulkanDriverManager`/`SessionFiles`/`BwrapSpawner`, with `versionCode` bumped.
4. **End to end:** Steam Big Picture renders, user signs in, launches a game, a frame is on screen.
   (A game/app rendering through the ICD already works — vkcube/glxgears — so once the client's own
   UI tools are satisfied, the game path is the proven part.)

`docs/development/tensor-g5-powervr-state.md` §40–§55 is the fine-grained record of every measurement
above; `docs/development/pvr-re/hybshim/gbm/README.md` documents the shim's build/install recipe.
