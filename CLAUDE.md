# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

DroidDeck is an Android app that runs the **SteamOS experience on Android handhelds**: it executes a
real ARM64 glibc Linux rootfs under **proot**, runs Valve's Steam client in **gamescope** (a Wayland
compositor), and shows it in an Android `SurfaceView`. It is not an emulator — the guest is the
runtime's own ARM64 userland, and Windows games run through Valve's ARM64 Proton. Single Gradle
module (`:app`), Kotlin + Jetpack Compose, with a sizeable C/C++ native layer.

## Build and test

**JDK 17 is required.** A newer default JDK (25 here) makes Gradle die with
`Unsupported class file major version 69`. Point `JAVA_HOME` at a 17 JDK, e.g.:

```sh
JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew assembleDebug --console=plain
JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew test --console=plain          # JVM + Robolectric unit tests
JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew test --tests "com.droiddeck.launcher.gpu.GpuVendorTest"
```

Both `debug` and `release` build types are signed with the public **AOSP testkey**
(`keystore/testkey.p12`, password `android`), so a debug and a release APK install over each other —
no uninstall needed. `assembleDebug` is the fast loop; the debug build additionally exposes the agent
control surface (below).

`tools/build_local.sh` produces the **CI-equivalent** APK: it cross-compiles the glibc ARM64 preload
libraries in Docker and stages the gitignored assets (`app/src/main/assets/linuxfs/`,
`droiddeck-esync/`, `pulseaudio.tzst`). Use it when a change touches the guest userland or those
assets; a plain `./gradlew assembleDebug` cannot rebuild them. It needs Docker, Java 17, the Android
SDK/NDK, `zstd`, and `gh` (for the pinned Gamescope/wlroots/esync release assets). Install with
`tools/deploy_local.sh`.

## Controlling a running session from the host (debug APKs)

The debug APK exposes a ContentProvider at `content://com.droiddeck.launcher.agent` and an Activity to
start sessions, both protected by Android's shell-only `DUMP` permission (absent from release builds).
Use `tools/droiddeckctl`:

```sh
tools/droiddeckctl start steam        # or: start desktop
tools/droiddeckctl wait ready --timeout 90
tools/droiddeckctl state --json
tools/droiddeckctl logs latest ./session-artifacts
tools/droiddeckctl stop
```

`SessionActivity` itself is **not exported**, so `am start` on it is refused. To start a session from
raw adb, use the agent trampoline:
`am start -n com.droiddeck.launcher/.agent.AgentStartActivity --es request <base64-json>`.
Session artifacts land in `Download/DroidDeck/<session-id>/` (`session.log`, `app.log`,
`events.jsonl`, and a `steam/` directory of the client's own logs).

## Architecture

**Process model.** `SessionService` is a foreground service that *owns* the running session — it is
deliberately not tied to the activity, because Android demotes a process the moment it loses its last
visible activity and the low-memory killer would then reap the guest. The activity comes and goes on
top of it; `wayland/CompositorHost` keeps a single process-wide compositor alive and merely swaps the
`Surface` it presents into as the activity re-creates. Do not start a second compositor.

**Session bring-up** (`session/SessionService.kt`, `runtime/`, `wayland/`):
1. `runtime/LinuxRuntime` builds the proot invocation and its bind mounts (this is where the guest
   sees `/vendor`, `/system`, the driver dir, etc.); `runtime/LinuxRuntimeInstaller` installs the
   rootfs tarball; `runtime/ProotFastPath` decides whether the seccomp fast-path shim
   (`libblfastpath.so`) goes into the guest's `/etc/ld.so.preload`.
2. `wayland/WaylandCompositor` (JNI → `app/src/main/cpp/waylandcomp`) owns the output Surface and the
   Wayland socket; gamescope connects to it as a nested compositor.
3. The session script (`tools/linuxfs/overlay/usr/local/bin/droiddeck-*`) launches gamescope, then
   Xwayland, then Steam.

**GPU driver management** (`gpu/`). Linux Vulkan drivers are shipped as assets and installed into the
guest's `files/linux_vulkan_drivers/`; the session passes the chosen one to the guest as
`BL_VK_DRIVER`. `LinuxVulkanDriverManager.installBundled` trusts an existing install only when the
extracted `meta.json`'s `installedByVersionCode` equals the running APK's `BuildConfig.VERSION_CODE` —
so **a changed driver asset with an unchanged `versionCode` never reaches the device.** Bump
`versionCode` in `app/build.gradle` whenever the bundled driver changes. `GpuInfo`/`DriverPairs`
decide which driver a device gets.

**Other subsystems:** `input/` (controllers, on-screen pad, touchpad gestures, fake input),
`audio/` (PulseAudio daemon + DirectAudioRelay), `frontend/` (Steam library, shortcuts, cover art),
`store/` (Flathub), `update/` (in-app updater).

**Native layer** (`app/src/main/cpp/`, NDK/CMake): `waylandcomp` (the compositor), `fakeinput_steam.cpp`,
`framegen`, `scanout`, `adrenotools`, `sdnative`, `deviceinfo`, `termux`, and `thirdparty/dxbc`.

**Host tooling** (`tools/`): `linuxfs/` is the guest filesystem overlay plus the glibc preload shims
(`tools/linuxfs/preload/*.c`, built into `libblsession.so`); `gamescope/`, `wlroots/`, `proot/`,
`fex/` carry patches applied to pinned upstream releases that **CI builds and publishes as release
assets** — a local checkout does not rebuild them. `tools/release/` handles signing and the update
catalog.

## Non-obvious constraints

- **`targetSdkVersion 28` is deliberate.** An app on targetSdk 29+ runs in a domain that may not
  execute a file out of its own data directory, and executing a rootfs is the whole job. Lint's
  `ExpiredTargetSdkVersion` is disabled for this reason; every other lint check stays on.
- **Some assets are gitignored and CI-built** (`app/src/main/assets/linuxfs/`, `droiddeck-esync/`); a
  fresh clone will not have them until `tools/build_local.sh` or CI stages them.
- **The in-app updater only sees upstream releases.** `update/AppUpdates.kt` reads a catalog CI
  publishes to `Droid-Deck/DroidDeck-CI`; a fork's Actions run or a local APK never reaches it.
- **Release pipeline:** `release.yml` only runs from `main` and refuses a tag that does not match
  `versionName` in `app/build.gradle`. Version lives there (`versionCode`/`versionName`), bumped by hand.

## Development notes

`docs/development/` holds the deep records — read the relevant one before working in an area.
`tensor-g5-powervr-state.md` is the authoritative, sectioned log of the (large) PowerVR/Tensor G5
bring-up effort; `google-tensor-support.md`, `steam-display-scaling.md`, `steam-focus-debugging.md`,
`proot-performance.md`, and `pvr-re/` cover their respective areas. `docs/agent-control.md` documents
the agent control surface in full.
