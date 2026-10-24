# The diagnostic harness: instruments that prove themselves

Date: 2026-10-24
Status: design, approved for planning

## Why this exists

The PowerVR bring-up has been slowed not by hard problems but by four repeating failures in how
evidence was gathered. Each has a named, recorded cost:

1. **An instrument was trusted before it was validated.** `vkp_pitch_dump` routed a 2404x1080 frame
   through `vkp_image_readback()`, a helper built for a 256x256 cursor. Three conclusions were
   published on its output and all three were retracted (state doc §65-68). The same class of defect
   was reported again in §62 ("a fourth observer defect found").
2. **What was measured was not what was shipped.** §36 exists because the build under test was not
   the build on the device. §63 had to establish, by hand, which ICD each process had actually
   mapped, because the installed file's hash is not proof of a load.
3. **A contrast that could not discriminate.** §63's A/B differed by one log line; a survival
   difference was recorded and then withdrawn.
4. **The device left armed.** Sessions 33-34 ran after a rollback command that had failed mid-USB
   flap, and the session record cannot say what was installed.

A fifth, cheaper lesson sits at §73: a hypothesis was written up and then refuted by a log line
already on disk, in a file already pulled.

And the constraint that shapes all of it: **no screenshots.** Images exhaust the provider context.
Every instrument here emits text. `screencap` is a permitted *data source* when explicitly
requested, decoded in-process, never stored as an image and never read as one.

## Purpose

Turn a long unattended session into evidence that is (a) free of the four failures above, (b) cheap
enough to keep for a whole run, and (c) searchable before a hypothesis is written, not after.

The first goal it serves: **verify the display path (§74-76) on hardware** - whether it presents, and
whether the picture is correct - with every claim traceable to a calibrated instrument.

## Part 1: the instrument registry and the calibration gate

### The rule

No instrument's output may be cited unless its calibration passed **in the same run** and the result
appears in that run's `manifest.json`. Output from a failed or absent calibration is written under
`untrusted/` and the analysis layer stamps any finding that depends on it **UNTRUSTED**.

This is the single behavioural change that addresses failure 1. It is enforced by the analysis layer,
not by discipline - a finding whose evidence cites an `untrusted/` artifact cannot be emitted.

### Registry

`tools/diag/instruments.toml` declares each instrument:

| Field | Meaning |
|---|---|
| `name` | stable id, used in findings |
| `kind` | `guest-so`, `host-tool`, `icd-log`, `compositor-log`, `frame-digest`, `panel-digest`, `snapshot` |
| `build` | how to rebuild it, and where the binary lands |
| `identity` | how a run proves the built artifact is the one that ran (hash, printed signature, or mapped inode) |
| `calibration` | the known-answer test and its tolerance |
| `cost` | bytes/minute at default settings, so a run can budget |

Initial entries: `crashcatch`, `mmwatch`, `symwatch`, `icd-wsi-log`, `icd-diag-log`,
`compositor-log`, `frame-digest`, `panel-digest`, `proc-watch`, `x-inspect`, `cdp-observer`,
`device-report`, `snapshot`.

### Calibrations

Each is a known-answer test, run at the start of every run, and its failure is loud:

- **crashcatch** - a probe deliberately faults with a known `si_addr`; the report must exist, carry
  that address, and symbolize to the known function through the report's own maps.
- **frame digest** - a calibrator renders a known pattern (gradient plus a glyph block) at the
  session's true resolution, and the digest must recover band means, the zero-row count and the
  luminance grid within stated tolerance. **This is the test that the cursor-helper class of bug
  fails**, and it runs at full resolution, not at whatever size the helper was designed for.
- **mmwatch / symwatch** - the calibrator performs one known `mmap` and one known `dlopen`; both
  must appear in the log with the expected caller.
- **icd-wsi-log** - the captured stderr must contain the ICD's own build signature; absence means the
  log is not from the ICD that was deployed.
- **panel digest** - a solid-colour frame is composed, `screencap` decoded, and the recovered band
  means must match the colour. Also proves the screencap path and the colour space agree. This is
  the one calibration that needs a `screencap`, so it runs **only when the run declares it will use
  the panel digest**, and the run records that a screencap was taken at its start.

### Manifest

`manifest.json` per run records: label and hypothesis, git commit, APK versionCode, every instrument
with its identity and calibration verdict, every deployed file with its hash and the hash expected
from the build, and the arm definition (what was changed). It is written **before** the run starts
and stamped at the end with the teardown verdict.

## Part 2: proof of what ran

Failure 2. Every run records, per graphics process (gamescope, Xwayland, steam, steamwebhelper and
its children, the CEF GPU process):

- the `libvulkan_powervr.so`, `libhybris.so`, `q.so`, and any GBM/shim library **actually mapped**,
  identified by dev:inode:size from `/proc/<pid>/maps` - the technique §63 established, automated;
- the driver's own printed build signature, where the driver has one;
- the APK `versionCode` and build label from the agent.

A run whose mapped set does not match the deployed set is marked **TAINTED**. A finding may not cite
a TAINTED run as its sole support.

**Where "deployed" comes from matters.** For APK-bundled artifacts the expected hash is read from the
build's own record (`dist/build_id.json` for the driver and ICD, `BuildConfig` for the APK). For
hand-pushed instrumentation the expected hash is the one `diag run` wrote into the manifest at deploy
time, and the maps check proves the push landed - it cannot prove the binary was built from the tree.
So a run also records each pushed artifact's hash **as built**, and `analyze` warns when a pushed
artifact has no build record. This is the limit the §36 lesson imposes: the harness can close the
installed-vs-loaded gap, and can only flag the built-vs-pushed gap.

## Part 3: frames as text, at true resolution

Two producers, one schema. This is the answer to "I cannot read images, and there is no substitute
for what is on screen".

### Schema (per sample)

```json
{"t_mono": 12.345, "source": "scene|panel", "w": 2404, "h": 1080, "fmt": "RGBA8",
 "bands": [[mean, var, zero_rows], ...],            // 32 horizontal bands
 "row_runs": 12, "zero_rows": 24, "zero_runs": [[0,4],[1060,20]],
 "rl": [r400, r401],                                 // run-length profile, coarse histogram
 "grid": [[l, ...], ...],                            // 16x16 mean luminance, 0-255
 "hash": "sha256:...",                               // over the sampled rows only
 "ascii": "..."}                                     // 48x20 ASCII luminance map, at low rate only
```

Roughly 1-2 KB per sample; the ASCII map is emitted at a much lower rate than the numbers.

### Compositor digest

After a scene is composed, `vkp_frame_digest()` reads it back **into a staging image sized from the
image's real extent and format** - never through a fixed-size helper - and emits one record per
sampled frame. Gated by `BL_FRAME_DIGEST=1`, with a level for numeric-only versus numeric+ASCII.

**Cadence: adaptive.** Every frame for the first 120 frames, then every Nth, **plus** a sampler that
fires whenever the picture changes character (band-mean deltas above a threshold, a new zero-run
appearing, or the frame going static for longer than a set interval). A whole session stays bounded
while a transient fault is still caught.

### Panel digest

`screencap` on explicit request only, decoded straight into the same schema. The image bytes are
deleted after decoding. This preserves the only independent view of the panel - the thing that ended
the §65-68 dead end - without ever handling an image.

### Why the shared schema matters

§70's open question - the panel is corrupt while SurfaceFlinger's own capture is coherent - becomes
`diag compare --frame scene:A panel:B`: a text diff of two records. The ASCII map answers "is there
a recognisable picture at all" in a few hundred tokens.

## Part 4: the run harness

`tools/diag/` is the front door. It is a transaction, not a sequence of commands.

```
diag doctor                 device cleanliness and readiness; refuses a run on a dirty device
diag run --label ... --arm  one declared variable; deploy verified; teardown verified
diag prior "<question>"     search every existing run for evidence, with file:line
diag analyze <run>          timeline, symbolization, findings with citations
diag compare <A> <B>        refuses unless the arms differ only in the declared variable
diag export <run> [dest]    stage a text-only bundle safe to attach to a report
```

- **doctor** enumerates every artifact the harness knows how to deploy, checks whether each is
  present on the device, and compares against stock. Anything left over blocks the run. This is
  failure 4, prevented rather than described.
- **run** takes `--arm`, which names the one thing being varied. Deployment is hash-verified in both
  directions; teardown runs in a `finally` and is verified; if the device is not restored the run is
  TAINTED and the next `doctor` will say so. This is failure 3: an arm that varies two things, or
  whose difference is one log line, is visible in the manifest before the run.
- **prior** greps the indexed corpus (including `/d/GUH/audit` and every session folder) and prints
  matches with `file:line`. The §73 lesson, mechanised.
- **analyze** correlates clocks across sources (the record shows guest wall clock is faked; the
  anchor line already exists in the session script and becomes mandatory), symbolizes crashes
  against the run's own maps, and emits a findings table in which **every row cites an artifact and
  a line**. A row citing an untrusted artifact is marked UNTRUSTED; a row citing only a TAINTED run
  is marked TAINTED.
- **compare** diffs two manifests first and refuses if more than the declared variable differs.

## Part 5: in-app additions

Both in-app and host-side was chosen, so these are in scope:

- **Agent `snapshot`** - one call gathers, as text, into the session folder: `dumpsys
  SurfaceFlinger` (our layers, composition type, buffer geometry), `dumpsys input` and
  `dumpsys display`, `/proc` for every graphics process, GPU sysfs where readable, guest `/proc`,
  and logcat tails. The "capture everything at this instant" button, callable mid-run.
- **Agent `mark`** - writes a labelled marker into `events.jsonl` and the guest log at the same
  moment, so a host action and the guest timeline land on one correlation axis.
- **Frame digest control** - the agent can turn the digest on, off, and set its level per run, so an
  unattended harness can raise the rate around a suspected moment.

These require an APK rebuild (`assembleDebug`, JDK 17 present). The Kotlin surface additions follow
the existing `SessionPart`/provider patterns.

## Retention

Per run: metrics, manifest, text logs and symbolization kept indefinitely. Raw pixel intermediates,
if any, capped at 200 MB per run and rotated, with derived text retained. A ten-hour run stays
bounded and greppable. The existing 638 MB corpus under `/d/GUH/audit` is indexed, not deleted.

## Toolchain reality

- APK-native: `./gradlew assembleDebug` with JDK 17 (`~/.jdks/jdk-17.0.20+8`). Java 25 on PATH will
  fail the build; the harness pins JAVA_HOME.
- Guest instrumentation (`.so`): built in WSL, where zig 0.13.0 is installed at
  `~/zig-linux-x86_64-0.13.0`, following `rig/build.sh`'s target/flags.
- **Docker is not installed**, so `tools/build_local.sh` cannot stage the gitignored linuxfs assets.
  Nothing in this design may depend on it. If an implementation step turns out to, it is a blocker to
  raise immediately rather than work around by hand-copying (rig rule 7).

## Non-goals

- No graphical output of any kind. No image is written, stored, or read.
- No change to the display path's behaviour. This harness observes; it does not fix §74-76.
- No restructuring of `droiddeckctl`'s existing commands; `diag` calls them.
- No new guest-side tracing beyond the calibration probes and the digest.

## Testing

- Every calibration is itself tested off-device against a synthetic producer, so a broken calibration
  is caught without a phone.
- The frame digest's known-answer test runs in unit tests at several resolutions, including
  2404x1080 and a deliberately padded stride - the shapes that broke the cursor helper.
- `diag compare`'s refusal logic is tested with manifests that differ in one field and in two.
- The manifest schema is tested against a recorded run fixture.
- Source assertions in the style of `tools/tests/test_pvr_icd.py` guard the seams: that the digest
  sizes its staging image from the real extent, and that no analysis path can cite an untrusted
  artifact.

## Success criteria

The display path is either verified or precisely localised on hardware, and every claim in that
verdict cites a calibrated instrument in a manifest that also proves which binaries ran. If the
session produces a finding, `diag analyze` can print the artifact and line behind it. If the session
is cut short, `diag doctor` can still say exactly what is on the device.
