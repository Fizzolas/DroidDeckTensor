# The PowerVR test rig

**Why this exists.** Every PowerVR fault so far was diagnosed after the fact, from a crash report and
whatever log happened to survive, and that has repeatedly been not enough. The clearest case: the HAL's
PLT slots point into `libhybris.so` when the HAL opens, and into unmapped memory when the client dies -
and there is no record of what happened in between, so "was it unmapped, or never mapped in that
process?" cannot be answered. Reading the wreckage cannot answer questions about the minutes before the
wreck.

This is the standing instrumentation for the guest, kept out of the debugging path so it can be
preloaded wholesale rather than rebuilt per question.

## Layers

| Layer | File | What it records |
|---|---|---|
| **Memory map changes** | `mmwatch.c` | every `mmap`/`munmap`/`mprotect`, with caller - the layer that answers "when did that region go away?" |
| **Symbol resolution** | `symwatch.c` | every `dlsym`/`android_dlsym`/`dlopen`, and libhybris's hook callback - "what did this symbol resolve to, and when" |
| **Faults** | `crashcatch.c` | fatal-signal catcher that survives breakpad (interposes `sigaction`), writes registers, a stack scan and `maps` |
| **Hook callback** | in the ICD, `PVR_ICD_DEBUG` | what the HAL asks libhybris to resolve, before the ICD calls the HAL's `open` |

## Rules learned the hard way

1. **`LD_PRELOAD` is the delivery route, not `/etc/ld.so.preload`.** `SessionFiles.kt` rewrites
   `/etc/ld.so.preload` at every session start, so anything put there lasts one session. `LD_PRELOAD`
   set on the launch command reaches children and is not touched.
2. **Gate a catcher on `comm`.** Several of Steam's helpers take faults on purpose and recover; a
   handler that catches those turns a recovered fault into a kill. `CRASHCATCH_ALL=1` overrides.
3. **Capture the link register first.** Any call made before reading `x30` (`dlsym`, `open`, even a
   `maps` parse) clobbers it; an interposer that reads it late reports a caller that is unmapped.
4. **Do the work before probing memory.** Counting mappings lazily, mid-report, re-enters the
   instrumented `mmap` and corrupts the report.
5. **The HAL is loaded by libhybris, not glibc** - `dladdr` cannot see it. Its base comes from
   `mod->methods->open` (which is `vulkan_device_open` at file offset `0x1b9ee0`).
6. **`libhybris.so` and its diagnostics are silent by default** (`#if defined(DEBUG)` in `logging.h`,
   and a `WARN` minimum level). A `-DDEBUG` build of `libhybris.so` breaks gamescope outright - do not
   ship or deploy one. Diagnostics belong in the ICD instead, writing to their own file.
7. **Never hand-copy over a shipped file.** Put instrumentation in `files/linuxfs/tmp/` and remove it.
   Copying a library over `files/linux_vulkan_drivers/...` invalidated a whole run earlier today.
8. **Identify the process.** Multiple processes load the ICD (`gamescope`, `Xwayland`, `steam`, ...);
   absolute addresses from different ones are not comparable. Every line carries pid and comm.

## Use

```sh
bash docs/development/pvr-re/rig/build.sh          # builds the shims into dist/
# then deploy: push dist/* to the guest, set LD_PRELOAD on the session's steam launch
```

Outputs land in `/tmp/` inside the guest (`/tmp/mm.log`, `/tmp/sym.log`, `/tmp/crash.txt`), which maps
to `files/linuxfs/tmp/` from the app's side.

## First result (session 2026-10-10-127)

With the rig live for a whole session (crashcatcher + mmwatch + symwatch):

* **3886 memory events** captured across `Xwayland` (2102), `gamescope` (1162), `steam` and the
  rest - every `mmap`/`munmap`/`mprotect` with pid, comm and caller.
* The crash was caught, and the crashed page had **no mm event touching it at all** - not an `mmap`
  that was later `munmap`ed, but a region that was **never mapped** in that process.
* `si_addr` ended in `fc028` in every crash recorded today (`0x7b582dc028`, `0x7ca74fc028`,
  `0x76a19fc028`), i.e. a **fixed offset** that varies with some base, not a random value. So the
  address is computed, not garbage.

That is the shape of a wrong computed address, and it is exactly the kind of question the crash
report alone could not answer.

**Two deployment facts this cost time to learn, now recorded so they do not cost it again:**

* **`tools/linuxfs/overlay/` is the master, not `app/src/main/assets/linuxfs/`.** The build writes
  `overlay/*` into the assets, so editing the asset is editing a generated file and the next build
  reverts it silently (a 49976-byte file becomes 49279 again).
* **The APK's asset merge is cached.** Deleting the apk and re-running is not enough; the change has
  to originate in `overlay/` (and can need `--rerun-tasks`) before it reaches the apk.
