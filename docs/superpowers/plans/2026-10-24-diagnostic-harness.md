# Diagnostic Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a harness in which no instrument's output may be cited unless it proved itself in the same run, frames are evidence as text, and a device session cannot leave the device in an unknown state.

**Architecture:** A Python package `tools/diagkit/` owns manifests, calibration verdicts, frame digests, the device transaction and the analysis layer; a thin `tools/diag` script is the CLI. Frame digests exist twice - once in Python (for `screencap` and for tests) and once in dependency-free C inside the compositor - and are held to **byte-identical integer output** by a parity test, because two producers that disagree are two instruments that cannot be compared. Kotlin agent methods (`snapshot`, `mark`) expose live device state and a shared correlation clock.

**Tech Stack:** Python 3.12 stdlib (unittest; no numpy/PIL - CI is `ubuntu-24.04` running `python3 -m unittest discover -s tools/tests`), dependency-free C11 for the digest core, NDK/CMake for the compositor, Kotlin/Compose for the app, `adb`.

**Spec:** `docs/superpowers/specs/2026-10-24-diagnostic-harness-design.md`

## Global Constraints

- **No image is ever written, stored, or read.** `screencap` is a permitted data source **only when a run declares it**, is decoded in-process, and its bytes are never persisted.
- Python harness code must be **stdlib-only** (`zlib`, `json`, `hashlib`, `argparse`, `subprocess`, `unittest`).
- Frame digest output is **integers only**, so Python and C can be compared exactly.
- The digest must take `pitch` from the producer. **Never assume `pitch == width * 4`** - that assumption is what produced the `vkp_image_readback` defect (state doc §68).
- Push instrumentation only into `files/linuxfs/tmp/`; never overwrite a shipped file (rig rule 7). The app rewrites `/etc/ld.so.preload` every session, so delivery is `LD_PRELOAD` on the launch command (rig rule 1).
- Bump `versionCode` in `app/build.gradle` whenever bundled assets change.
- JDK 17 for Gradle: `JAVA_HOME=~/.jdks/jdk-17.0.20+8`.
- Guest `.so` and the guest calibrator build in WSL with `~/zig-linux-x86_64-0.13.0/zig`, target `aarch64-linux-gnu.2.31` (rig) as `rig/build.sh` does.
- Nothing may depend on `tools/build_local.sh` (Docker is not installed on this host).
- Tests go in `tools/tests/test_*.py`, `unittest` style, and are discovered by CI from the repo root.

---

## File Structure

**Created**

| Path | Responsibility |
|---|---|
| `tools/diagkit/__init__.py` | package marker, version |
| `tools/diagkit/manifest.py` | manifest schema, hashing, trust verdicts |
| `tools/diagkit/frames.py` | the digest algorithm, pattern generator, `screencap` decode |
| `tools/diagkit/instruments.toml` | instrument registry data |
| `tools/diagkit/registry.py` | loads and validates the registry |
| `tools/diagkit/device.py` | adb calls layered on `tools/adb_device.py` |
| `tools/diagkit/doctor.py` | device inventory and cleanliness verdict |
| `tools/diagkit/runs.py` | the run transaction: deploy, verify, teardown |
| `tools/diagkit/analyze.py` | timeline, symbolization, cited findings |
| `tools/diagkit/prior.py` | corpus index and search |
| `tools/diagkit/compare.py` | arm-isolation check and digest diff |
| `tools/diagkit/cli.py` | argparse front end |
| `tools/diag` | executable shim |
| `app/src/main/cpp/waylandcomp/src/frame_digest.c` | dependency-free digest core (C) |
| `app/src/main/cpp/waylandcomp/src/frame_digest.h` | its interface |
| `app/src/main/cpp/waylandcomp/src/frame_digest_selftest.c` | host self-test driver |
| `docs/development/pvr-re/rig/calibrate.c` | guest calibration probe |
| `tools/tests/test_diag_manifest.py` | manifest tests |
| `tools/tests/test_diag_frames.py` | digest tests |
| `tools/tests/test_diag_frames_parity.py` | Python/C parity |
| `tools/tests/test_diag_registry.py` | registry tests |
| `tools/tests/test_diag_doctor.py` | doctor tests (fake adb) |
| `tools/tests/test_diag_runs.py` | transaction tests (fake adb) |
| `tools/tests/test_diag_analyze.py` | analysis and citation tests |
| `tools/tests/test_diag_prior.py` | corpus search tests |
| `tools/tests/test_diag_compare.py` | arm isolation tests |
| `tools/tests/test_diag_native.py` | source assertions on the C digest and its call site |

**Modified**

| Path | Change |
|---|---|
| `app/src/main/cpp/waylandcomp/CMakeLists.txt` | add `src/frame_digest.c` |
| `app/src/main/cpp/waylandcomp/src/vk_present.c` | add `vkp_frame_digest()` and the adaptive sampler |
| `app/src/main/cpp/waylandcomp/src/vk_present.h` | declare it |
| `app/src/main/java/com/droiddeck/launcher/agent/AgentBridgeProvider.kt` | `snapshot`, `mark`, `digest` methods |
| `docs/agent-control.md` | document the new methods |
| `docs/development/tensor-g5-powervr-state.md` | a section recording the harness |

---

## Phase 1 - Harness core

### Task 1: Manifest schema and trust verdicts

**Files:**
- Create: `tools/diagkit/__init__.py`, `tools/diagkit/manifest.py`
- Test: `tools/tests/test_diag_manifest.py`

**Interfaces:**
- Produces: `new_manifest(label: str, hypothesis: str, arm: dict, commit: str, apk_version_code: int) -> dict`; `write_manifest(run_dir: Path, m: dict) -> Path`; `read_manifest(run_dir: Path) -> dict`; `stamp_instrument(m: dict, name: str, identity: str, calibration_ok: bool, detail: str) -> None`; `stamp_deploy(m: dict, path: str, expected_sha: str, pushed_sha: str) -> None`; `mark_tainted(m: dict, reason: str) -> None`; `trust(m: dict) -> str` returning `clean` / `tainted`; `instrument_trusted(m: dict, name: str) -> bool`; `sha256_file(path: Path) -> str`; `sha256_bytes(data: bytes) -> str`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, json, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('manifest', TOOLS / 'diagkit/manifest.py')
manifest = importlib.util.module_from_spec(spec); spec.loader.exec_module(manifest)


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp, True))
        self.m = manifest.new_manifest('display path', 'the layer path presents', {'name': 'icd', 'value': 'A'}, 'abc123', 40)

    def test_a_new_manifest_is_clean(self):
        self.assertEqual('clean', manifest.trust(self.m))

    def test_a_tainted_run_is_not_clean(self):
        manifest.mark_tainted(self.m, 'shim left on device')
        self.assertEqual('tainted', manifest.trust(self.m))
        self.assertEqual(['shim left on device'], self.m['taint']['reasons'])

    def test_an_instrument_must_have_passed_its_calibration_to_be_trusted(self):
        manifest.stamp_instrument(self.m, 'crashcatch', 'sha256:aa', False, 'known address not reported')
        self.assertFalse(manifest.instrument_trusted(self.m, 'crashcatch'))
        manifest.stamp_instrument(self.m, 'crashcatch', 'sha256:aa', True, 'ok')
        self.assertTrue(manifest.instrument_trusted(self.m, 'crashcatch'))

    def test_an_instrument_never_stamped_is_not_trusted(self):
        self.assertFalse(manifest.instrument_trusted(self.m, 'mmwatch'))

    def test_a_deploy_whose_pushed_hash_differs_taints_the_run(self):
        manifest.stamp_deploy(self.m, 'files/tmp/crashcatch.so', 'sha256:aa', 'sha256:bb')
        self.assertEqual('tainted', manifest.trust(self.m))
        self.assertIn('files/tmp/crashcatch.so', self.m['taint']['reasons'][0])

    def test_a_matching_deploy_does_not_taint(self):
        manifest.stamp_deploy(self.m, 'files/tmp/crashcatch.so', 'sha256:aa', 'sha256:aa')
        self.assertEqual('clean', manifest.trust(self.m))

    def test_a_manifest_round_trips_through_disk(self):
        manifest.stamp_instrument(self.m, 'frame-digest', 'sha256:cc', True, 'ok')
        path = manifest.write_manifest(self.tmp, self.m)
        self.assertTrue(path.is_file())
        self.assertEqual(self.m, manifest.read_manifest(self.tmp))

    def test_hashing_matches_hashlib(self):
        f = self.tmp / 'x'; f.write_bytes(b'hello')
        self.assertEqual('sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
                         manifest.sha256_file(f))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest tools.tests.test_diag_manifest -v` (from `D:/GUH/DroidDeck`; if module discovery complains, use `python3 -m unittest discover -s tools/tests -p 'test_diag_manifest.py' -v`)
Expected: FAIL - `ModuleNotFoundError` / `No such file or directory: 'tools/diagkit/manifest.py'`.

- [ ] **Step 3: Write the implementation**

```python
"""The record that says what ran, with what, and whether it may be believed.

A manifest is written before the run starts and stamped as the run proceeds. `trust()` is the
single place that decides whether a run's evidence may be cited: a run whose instrumentation was
pushed but not verified, or whose calibration failed, is TAINTED and cannot stand alone as proof.
This exists because the record shows runs narrated as one thread that were not (state doc §36),
and a rollback that failed mid-USB-flap leaving the device armed (§63).
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

SCHEMA = 1
FILENAME = 'manifest.json'


def sha256_bytes(data: bytes) -> str:
    return 'sha256:' + hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return 'sha256:' + h.hexdigest()


def new_manifest(label, hypothesis, arm, commit, apk_version_code):
    return {
        'schema': SCHEMA,
        'label': label,
        'hypothesis': hypothesis,
        'arm': arm,
        'commit': commit,
        'apkVersionCode': apk_version_code,
        'instruments': {},
        'deploys': [],
        'taint': {'reasons': []},
        'startedAt': None,
        'finishedAt': None,
        'teardown': None,
    }


def mark_tainted(m, reason):
    if reason not in m['taint']['reasons']:
        m['taint']['reasons'].append(reason)


def stamp_instrument(m, name, identity, calibration_ok, detail):
    m['instruments'][name] = {
        'identity': identity,
        'calibration': 'passed' if calibration_ok else 'failed',
        'detail': detail,
    }


def stamp_deploy(m, path, expected_sha, pushed_sha):
    m['deploys'].append({'path': path, 'expected': expected_sha, 'pushed': pushed_sha})
    if expected_sha and pushed_sha and expected_sha != pushed_sha:
        mark_tainted(m, f'{path}: pushed {pushed_sha} but expected {expected_sha}')
    elif expected_sha and not pushed_sha:
        mark_tainted(m, f'{path}: nothing pushed where {expected_sha} was expected')


def instrument_trusted(m, name):
    entry = m['instruments'].get(name)
    return bool(entry) and entry['calibration'] == 'passed'


def trust(m):
    return 'tainted' if m['taint']['reasons'] else 'clean'


def write_manifest(run_dir, m):
    path = Path(run_dir) / FILENAME
    path.write_text(json.dumps(m, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    return path


def read_manifest(run_dir):
    return json.loads((Path(run_dir) / FILENAME).read_text(encoding='utf-8'))
```

`tools/diagkit/__init__.py`:

```python
"""The diagnostic harness: instruments that prove themselves before they are believed."""
__version__ = '1.0'
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_manifest.py' -v`
Expected: PASS (8 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/__init__.py tools/diagkit/manifest.py tools/tests/test_diag_manifest.py
git commit -m "diag: the run manifest, and the one place that decides if evidence may be cited"
```

---

### Task 2: The frame digest, in Python

**Files:**
- Create: `tools/diagkit/frames.py`
- Test: `tools/tests/test_diag_frames.py`

**Interfaces:**
- Produces: `RAMP`; `digest_rgba(pixels: bytes, w: int, h: int, pitch: int | None = None, *, bands: int = 32, grid: int = 16, ascii_cols: int = 48, ascii_rows: int = 20, want_ascii: bool = False) -> dict`; `parse_screencap(raw: bytes) -> tuple[bytes, int, int, int]`; `pattern_rgba(w: int, h: int) -> bytes`.
- The returned dict has exactly: `w`, `h`, `bands` (list of `[mean, spread, zero_rows]`), `zero_rows`, `zero_runs` (list of `[start, length]`), `rl` (13-bucket run-length histogram, ints), `grid` (16 rows of 16 ints), `hash`, and `ascii` when `want_ascii`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frames', TOOLS / 'diagkit/frames.py')
frames = importlib.util.module_from_spec(spec); spec.loader.exec_module(frames)


def solid(w, h, rgba):
    return bytes(rgba) * (w * h)


class DigestTest(unittest.TestCase):
    def test_a_solid_frame_has_one_band_mean(self):
        d = frames.digest_rgba(solid(8, 8, (0x33, 0x33, 0x33, 0xff)), 8, 8)
        means = {b[0] for b in d['bands']}
        self.assertEqual(1, len(means))
        self.assertEqual((77 * 0x33 + 150 * 0x33 + 29 * 0x33) >> 8, means.pop())

    def test_a_tightly_packed_frame_and_a_padded_one_digest_identically(self):
        """The defect that cost §65-68: a producer's real row pitch is not width*4."""
        w, h, packed = 8, 4, 32
        rows = [bytes([i * 4, 0, 0, 255]) * w for i in range(h)]
        tight = b''.join(rows)
        padded = b''.join(r + b'\xde\xad\xbe\xef' * ((packed - w * 4) // 4) for r in rows)
        self.assertEqual(frames.digest_rgba(tight, w, h, pitch=w * 4),
                         frames.digest_rgba(padded, w, h, pitch=packed))

    def test_a_zero_row_is_reported_with_its_position_and_length(self):
        w, h = 4, 10
        rows = [bytes([9, 9, 9, 255]) * w for _ in range(h)]
        for i in (2, 3, 7):
            rows[i] = bytes(4 * w)
        d = frames.digest_rgba(b''.join(rows), w, h)
        self.assertEqual(3, d['zero_rows'])
        self.assertEqual([[2, 2], [7, 1]], d['zero_runs'])

    def test_the_hash_changes_when_a_pixel_changes(self):
        a = frames.digest_rgba(solid(8, 8, (1, 2, 3, 4)), 8, 8)
        b = frames.digest_rgba(solid(8, 8, (1, 2, 3, 5)), 8, 8)
        self.assertNotEqual(a['hash'], b['hash'])

    def test_the_ascii_map_is_the_requested_shape_and_is_monotonic(self):
        w, h = 64, 40
        px = bytearray()
        for y in range(h):
            v = int(255 * y / (h - 1))
            px += bytes([v, v, v, 255]) * w
        d = frames.digest_rgba(bytes(px), w, h, want_ascii=True)
        lines = d['ascii'].splitlines()
        self.assertEqual(20, len(lines))
        self.assertTrue(all(len(line) == 48 for line in lines))
        self.assertLess(frames.RAMP.index(lines[0][0]), frames.RAMP.index(lines[-1][0]))

    def test_a_truncated_buffer_is_refused_rather_than_guessed(self):
        with self.assertRaises(ValueError):
            frames.digest_rgba(b'\x00' * 10, 8, 8)

    def test_screencap_raw_is_decoded_by_its_own_header(self):
        import struct
        w, h = 3, 2
        raw = struct.pack('<III', w, h, 1) + bytes([1, 2, 3, 4]) * (w * h)
        px, pw, ph, fmt = frames.parse_screencap(raw)
        self.assertEqual((w, h, 1), (pw, ph, fmt))
        self.assertEqual(bytes([1, 2, 3, 4]) * 6, px)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_frames.py' -v`
Expected: FAIL - module not found.

- [ ] **Step 3: Write the implementation**

```python
"""A frame as text: the same record from the compositor and from the panel.

Why this exists: three findings in the state doc were built on a readback helper sized for a
256x256 cursor, and all three were retracted (§65-68). The rule that prevents a repeat is that a
digest is *checked against a known pattern* (see `calibrate`), and that it takes the producer's
real pitch instead of assuming a tightly packed row. The second producer is `screencap`, decoded
here and never stored: the panel is the only independent view and §70 is what settled a week of
argument with it.
"""
from __future__ import annotations

import hashlib
import struct

RAMP = ' .:-=+*#%@'

# Run lengths are bucketed so the profile is a fixed-size, integer record.
RL_BUCKETS = (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)


def _luma_table():
    return [(77 * i * 256 + 150 * i * 256 + 29 * i * 256) // 256 for i in range(256)]


def _row_means(pixels, w, h, pitch):
    """Per-row mean luminance and whether the row is wholly zero, in one pass over slices."""
    means, zeros = [], []
    for y in range(h):
        start = y * pitch
        row = pixels[start:start + w * 4]
        if len(row) != w * 4:
            raise ValueError(f'row {y} short: {len(row)} of {w * 4} bytes')
        sr = sum(row[0::4]); sg = sum(row[1::4]); sb = sum(row[2::4])
        means.append((77 * sr + 150 * sg + 29 * sb) // (256 * w))
        zeros.append(row.count(0) == len(row))
    return means, zeros


def _runs(flags):
    out, start = [], None
    for i, flag in enumerate(flags):
        if flag and start is None:
            start = i
        elif not flag and start is not None:
            out.append([start, i - start]); start = None
    if start is not None:
        out.append([start, len(flags) - start])
    return out


def _run_lengths(row, width):
    hist = [0] * len(RL_BUCKETS)
    run, prev = 0, None
    for x in range(width):
        px = row[x * 4:x * 4 + 4]
        if px == prev:
            run += 1
            continue
        if prev is not None:
            hist[_bucket(run)] += 1
        prev, run = px, 1
    if prev is not None:
        hist[_bucket(run)] += 1
    return hist


def _bucket(n):
    for i, cap in enumerate(RL_BUCKETS):
        if n <= cap:
            return i
    return len(RL_BUCKETS) - 1


def _cell_means(pixels, w, h, pitch, cols, rows):
    """Mean luminance of a cols x rows partition of the frame, as integers."""
    out = [[0] * cols for _ in range(rows)]
    for r in range(rows):
        y0, y1 = r * h // rows, (r + 1) * h // rows
        for c in range(cols):
            x0, x1 = c * w // cols, (c + 1) * w // cols
            total = 0
            for y in range(y0, y1):
                base = y * pitch + x0 * 4
                seg = pixels[base:base + (x1 - x0) * 4]
                sr = sum(seg[0::4]); sg = sum(seg[1::4]); sb = sum(seg[2::4])
                total += 77 * sr + 150 * sg + 29 * sb
            n = max(1, (y1 - y0) * (x1 - x0))
            out[r][c] = total // (256 * n)
    return out


def digest_rgba(pixels, w, h, pitch=None, *, bands=32, grid=16,
                ascii_cols=48, ascii_rows=20, want_ascii=False):
    pitch = pitch or w * 4
    if len(pixels) < pitch * (h - 1) + w * 4:
        raise ValueError(f'buffer of {len(pixels)} bytes cannot hold {w}x{h} at pitch {pitch}')
    means, zeros = _row_means(pixels, w, h, pitch)

    band_rows = []
    for b in range(bands):
        y0, y1 = b * h // bands, (b + 1) * h // bands
        chunk = means[y0:y1] or [0]
        mean = sum(chunk) // len(chunk)
        spread = max(chunk) - min(chunk)
        band_rows.append([mean, spread, sum(1 for z in zeros[y0:y1] if z)])

    hsh = hashlib.sha256()
    for y in range(h):
        hsh.update(pixels[y * pitch:y * pitch + w * 4])

    record = {
        'w': w, 'h': h,
        'bands': band_rows,
        'zero_rows': sum(1 for z in zeros if z),
        'zero_runs': _runs(zeros),
        'rl': _run_lengths(pixels[0:w * 4], w) if h else [0] * len(RL_BUCKETS),
        'grid': _cell_means(pixels, w, h, pitch, grid, grid),
        'hash': 'sha256:' + hsh.hexdigest(),
    }
    if want_ascii:
        cells = _cell_means(pixels, w, h, pitch, ascii_cols, ascii_rows)
        record['ascii'] = '\n'.join(
            ''.join(RAMP[min(len(RAMP) - 1, max(0, v * len(RAMP) // 256))] for v in row)
            for row in cells)
    return record


def parse_screencap(raw):
    """`adb exec-out screencap` (no -p): a 12-byte header, then RGBA. Never written to disk."""
    if len(raw) < 12:
        raise ValueError('screencap output too short for a header')
    w, h, fmt = struct.unpack('<III', raw[:12])
    need = 12 + w * h * 4
    if len(raw) < need:
        raise ValueError(f'screencap says {w}x{h} but carries {len(raw) - 12} bytes')
    return raw[12:need], w, h, fmt


def pattern_rgba(w, h):
    """The known answer: a horizontal gradient with a bright block whose rows are countable.

    A digest that sizes itself from the wrong extent, or reads a padded row as if it were tight,
    cannot reproduce this. That is the whole point of the pattern.
    """
    px = bytearray()
    for y in range(h):
        v = int(255 * y / max(1, h - 1))
        row = bytearray()
        for x in range(w):
            if h // 4 <= y < h // 2 and w // 4 <= x < w // 2:
                row += bytes([255, 255, 255, 255])
            else:
                row += bytes([v, v, v, 255])
        px += row
    return bytes(px)
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_frames.py' -v`
Expected: PASS (7 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/frames.py tools/tests/test_diag_frames.py
git commit -m "diag: frame digest in text, honouring the producer's pitch and refusing short buffers"
```

---

### Task 3: Calibration - the known answer gate

**Files:**
- Create: `tools/diagkit/calibrate.py`
- Test: `tools/tests/test_diag_calibrate.py` (new)

**Interfaces:**
- Consumes: `frames.digest_rgba`, `frames.pattern_rgba`.
- Produces: `check_digest(d: dict, w: int, h: int) -> tuple[bool, str]`; `check_crash_report(text: str, expected_addr: str) -> tuple[bool, str]`; `check_marks(text: str, expected: list[str]) -> tuple[bool, str]`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

frames = load('frames'); calibrate = load('calibrate')


def digest_of_pattern(w, h, **kw):
    return frames.digest_rgba(frames.pattern_rgba(w, h), w, h, **kw)


class DigestCalibrationTest(unittest.TestCase):
    def test_the_pattern_is_recovered(self):
        ok, detail = calibrate.check_digest(digest_of_pattern(320, 200), 320, 200)
        self.assertTrue(ok, detail)

    def test_a_digest_of_the_wrong_extent_is_refused(self):
        """A helper that read a 2404x1080 frame as 256x256 looked exactly like this."""
        d = digest_of_pattern(320, 200)
        ok, detail = calibrate.check_digest(d, 256, 256)
        self.assertFalse(ok)
        self.assertIn('extent', detail.lower())

    def test_a_padded_read_that_honours_pitch_passes(self):
        w, h = 320, 200
        tight = frames.pattern_rgba(w, h)
        rows = [tight[y * w * 4:(y + 1) * w * 4] for y in range(h)]
        padded = b''.join(r + b'\x00\x00\x00\x00' * 16 for r in rows)
        d = frames.digest_rgba(padded, w, h, pitch=w * 4 + 64)
        ok, detail = calibrate.check_digest(d, w, h)
        self.assertTrue(ok, detail)

    def test_a_uniform_frame_fails_because_the_block_is_missing(self):
        w, h = 320, 200
        d = frames.digest_rgba(bytes([128, 128, 128, 255]) * (w * h), w, h)
        ok, detail = calibrate.check_digest(d, w, h)
        self.assertFalse(ok)
        self.assertIn('pattern', detail.lower())

    def test_a_crash_report_is_checked_for_the_known_address(self):
        ok, _ = calibrate.check_crash_report('si_addr=0xdeadbe0  tid=7', '0xdeadbe0')
        self.assertTrue(ok)
        ok, detail = calibrate.check_crash_report('si_addr=0x1  tid=7', '0xdeadbe0')
        self.assertFalse(ok); self.assertIn('0xdeadbe0', detail)

    def test_marks_must_all_appear(self):
        ok, _ = calibrate.check_marks('a\nMARK-x\nb\n', ['MARK-x'])
        self.assertTrue(ok)
        ok, detail = calibrate.check_marks('nothing here\n', ['MARK-x'])
        self.assertFalse(ok); self.assertIn('MARK-x', detail)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_calibrate.py' -v`
Expected: FAIL - `diagkit/calibrate.py` missing.

- [ ] **Step 3: Write the implementation**

```python
"""Known-answer checks. An instrument that cannot reproduce the answer is not evidence.

Every check returns (ok, detail) and nothing else; the caller stamps the verdict into the run's
manifest. The digest check deliberately compares against the pattern's *known* structure rather
than against a stored golden file, so it cannot pass by reproducing a previous bug's output.
"""
from __future__ import annotations


def check_digest(d, w, h):
    if d.get('w') != w or d.get('h') != h:
        return False, f"extent is {d.get('w')}x{d.get('h')}, the frame being read is {w}x{h}"
    if d.get('zero_rows', 0) != 0:
        return False, f"pattern has no empty rows but the digest reports {d['zero_rows']}"
    grid = d.get('grid') or []
    if len(grid) != 16 or any(len(row) != 16 for row in grid):
        return False, 'grid is not 16x16'
    # Top row dark, bottom row bright: a vertical gradient the digest must see.
    top, bottom = sum(grid[0]) / 16, sum(grid[-1]) / 16
    if bottom - top < 40:
        return False, f'pattern not recovered: top {top:.0f}, bottom {bottom:.0f}'
    # The block sits in rows h//4..h//2, columns w//4..w//2, and must be the brightest cell.
    band = grid[4:8]
    inside = max(max(row[4:8]) for row in band)
    outside = max(max(row[0:4]) for row in band)
    if inside <= outside:
        return False, f'pattern block not recovered: inside {inside}, outside {outside}'
    return True, 'pattern recovered at the true extent'


def check_crash_report(text, expected_addr):
    if expected_addr.lower() not in text.lower():
        return False, f'known fault address {expected_addr} is not in the report'
    return True, f'report carries si_addr={expected_addr}'


def check_marks(text, expected):
    missing = [m for m in expected if m not in text]
    if missing:
        return False, 'missing marker(s): ' + ', '.join(missing)
    return True, 'all markers present'
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_calibrate.py' -v`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/calibrate.py tools/tests/test_diag_calibrate.py
git commit -m "diag: known-answer calibration for the digest, crash reports and markers"
```

---

### Task 4: The instrument registry

**Files:**
- Create: `tools/diagkit/instruments.toml`, `tools/diagkit/registry.py`
- Test: `tools/tests/test_diag_registry.py`

**Interfaces:**
- Produces: `load(path: Path | None = None) -> dict[str, dict]`; `validate(reg: dict) -> list[str]`; `cost_per_minute(reg: dict, names: list[str]) -> int`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('registry', TOOLS / 'diagkit/registry.py')
registry = importlib.util.module_from_spec(spec); spec.loader.exec_module(registry)


class RegistryTest(unittest.TestCase):
    def test_the_shipped_registry_loads_and_is_valid(self):
        reg = registry.load()
        self.assertEqual([], registry.validate(reg))
        for required in ('crashcatch', 'mmwatch', 'symwatch', 'frame-digest', 'panel-digest', 'snapshot'):
            self.assertIn(required, reg)

    def test_every_entry_declares_all_five_fields(self):
        for name, entry in registry.load().items():
            for field in ('kind', 'build', 'identity', 'calibration', 'cost'):
                self.assertIn(field, entry, f'{name} is missing {field}')

    def test_a_missing_field_is_reported_not_crashed(self):
        bad = {'x': {'kind': 'host-tool', 'build': '', 'identity': '', 'calibration': ''}}
        self.assertEqual(["x: missing 'cost'"], registry.validate(bad))

    def test_a_calibration_that_is_not_a_known_check_is_rejected(self):
        bad = {'x': {'kind': 'host-tool', 'build': 'b', 'identity': 'i', 'calibration': 'vibes', 'cost': 0}}
        errors = registry.validate(bad)
        self.assertTrue(errors and 'unknown calibration' in errors[0])

    def test_cost_sums_over_the_named_instruments(self):
        reg = registry.load()
        mine = ['crashcatch', 'mmwatch']
        self.assertEqual(sum(reg[n]['cost'] for n in mine), registry.cost_per_minute(reg, mine))

    def test_an_unknown_name_costs_nothing_and_is_ignored(self):
        self.assertEqual(0, registry.cost_per_minute(registry.load(), ['nope']))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_registry.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Write the registry data and loader**

`tools/diagkit/instruments.toml`:

```toml
# Every instrument the harness knows how to deploy or read, what proves it is the thing that ran,
# and the known answer it must reproduce before its output may be cited in a finding.
# `cost` is bytes per minute in the run's evidence directory at default settings.

[crashcatch]
kind = "guest-so"
build = "WSL: bash docs/development/pvr-re/rig/build.sh -> rig/dist/crashcatch.so"
identity = "sha256 of the pushed file, compared with /proc/<pid>/maps inode"
calibration = "known-fault-address"
cost = 4096

[mmwatch]
kind = "guest-so"
build = "WSL: bash docs/development/pvr-re/rig/build.sh -> rig/dist/mmwatch.so"
identity = "sha256 of the pushed file, compared with /proc/<pid>/maps inode"
calibration = "known-mmap"
cost = 65536

[symwatch]
kind = "guest-so"
build = "WSL: bash docs/development/pvr-re/rig/build.sh -> rig/dist/symwatch.so"
identity = "sha256 of the pushed file, compared with /proc/<pid>/maps inode"
calibration = "known-dlopen"
cost = 32768

[icd-wsi-log]
kind = "icd-log"
build = "WSL: bash docs/development/pvr-re/hybshim/build_all.sh -> hybshim/icd/libvulkan_powervr.so"
identity = "dist/build_id.json sha256 against /proc/<pid>/maps inode"
calibration = "build-signature"
cost = 16384

[icd-diag-log]
kind = "icd-log"
build = "WSL: bash docs/development/pvr-re/hybshim/build_all.sh (PVR_ICD_DEBUG=1 at runtime)"
identity = "build signature line in the captured stderr"
calibration = "build-signature"
cost = 16384

[compositor-log]
kind = "compositor-log"
build = "gradle: ./gradlew assembleDebug"
identity = "BuildConfig.VERSION_CODE from the agent state call"
calibration = "build-signature"
cost = 8192

[frame-digest]
kind = "frame-digest"
build = "gradle: ./gradlew assembleDebug (src/frame_digest.c + src/vk_present.c)"
identity = "build label printed in the first digest line"
calibration = "known-pattern"
cost = 10240

[panel-digest]
kind = "panel-digest"
build = "host: tools/diagkit/frames.py (no build)"
identity = "screencap header width/height must equal the session output size"
calibration = "known-pattern"
cost = 0

[proc-watch]
kind = "host-tool"
build = "none: pushed shell script"
identity = "sha256 of the pushed file"
calibration = "known-marker"
cost = 2048

[x-inspect]
kind = "host-tool"
build = "guest build: docs/development/pvr-re/rig (x11build)"
identity = "sha256 of the pushed file"
calibration = "known-marker"
cost = 4096

[cdp-observer]
kind = "host-tool"
build = "none: python script pushed into the guest"
identity = "sha256 of the pushed file"
calibration = "known-marker"
cost = 8192

[device-report]
kind = "snapshot"
build = "gradle: ./gradlew assembleDebug"
identity = "BuildConfig.VERSION_CODE"
calibration = "none"
cost = 2048

[snapshot]
kind = "snapshot"
build = "gradle: ./gradlew assembleDebug"
identity = "BuildConfig.VERSION_CODE"
calibration = "known-marker"
cost = 65536
```

`tools/diagkit/registry.py`:

```python
"""The instrument registry: what each instrument is, how to prove it ran, and what proves it works.

The registry is data so that a new instrument is a table edit, and so that `doctor` and `analyze`
agree on the same list. `validate` is public because the file is hand-edited.
"""
from __future__ import annotations

import tomllib
from pathlib import Path

FIELDS = ('kind', 'build', 'identity', 'calibration', 'cost')

# The calibration kinds `calibrate.py` can actually run. A name outside this set is a typo that
# would otherwise silently mean "never checked".
KINDS = {
    'known-pattern', 'known-fault-address', 'known-mmap', 'known-dlopen',
    'build-signature', 'known-marker', 'none',
}

DEFAULT = Path(__file__).with_name('instruments.toml')


def load(path=None):
    with open(path or DEFAULT, 'rb') as f:
        return tomllib.load(f)


def validate(reg):
    errors = []
    for name, entry in reg.items():
        for field in FIELDS:
            if field not in entry:
                errors.append(f"{name}: missing '{field}'")
        kind = entry.get('calibration')
        if kind is not None and kind not in KINDS:
            errors.append(f'{name}: unknown calibration {kind!r}')
    return errors


def cost_per_minute(reg, names):
    return sum(int(reg[n].get('cost', 0)) for n in names if n in reg)
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_registry.py' -v`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/instruments.toml tools/diagkit/registry.py tools/tests/test_diag_registry.py
git commit -m "diag: the instrument registry, as data, with the calibrations it can actually run"
```

---

### Task 5: `doctor` - the device cannot be worked on dirty

**Files:**
- Create: `tools/diagkit/device.py`, `tools/diagkit/doctor.py`
- Test: `tools/tests/test_diag_doctor.py`

**Interfaces:**
- Produces (device.py): `Adb` class with `__init__(adb: str, serial: str)`, `shell(cmd: str, timeout: int = 30) -> str`, `ls(path: str) -> list[str]`, `sha256(path: str) -> str | None`, `stat_size(path: str) -> int | None`.
- Produces (doctor.py): `ARTIFACTS` (list of dicts: `name`, `app_path`, `stock`); `inventory(adb: Adb) -> list[dict]` each with `name`, `present`, `sha256`, `stock`; `verdict(inventory) -> tuple[str, list[str]]`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

doctor = load('doctor')


class FakeAdb:
    """Stands in for a device: a path map and a hash per path."""
    def __init__(self, files):
        self.files = files
        self.calls = []
    def shell(self, cmd, timeout=30):
        self.calls.append(cmd)
        return ''
    def ls(self, path):
        return sorted(p for p in self.files if p.startswith(path))
    def sha256(self, path):
        return self.files.get(path)
    def stat_size(self, path):
        return None


class DoctorTest(unittest.TestCase):
    def test_a_stock_device_is_clean(self):
        inv = doctor.inventory(FakeAdb({}))
        state, reasons = doctor.verdict(inv)
        self.assertEqual('clean', state); self.assertEqual([], reasons)

    def test_an_instrument_left_behind_is_dirty(self):
        adb = FakeAdb({doctor.ARTIFACTS[0]['app_path']: 'sha256:leftover'})
        state, reasons = doctor.verdict(doctor.inventory(adb))
        self.assertEqual('dirty', state)
        self.assertIn(doctor.ARTIFACTS[0]['name'], reasons[0])

    def test_a_stock_file_restored_to_its_stock_hash_is_clean(self):
        a = doctor.ARTIFACTS[0]
        adb = FakeAdb({a['app_path']: a['stock']})
        self.assertEqual('clean', doctor.verdict(doctor.inventory(adb))[0])

    def test_a_modified_but_tracked_file_that_matches_stock_is_clean(self):
        a = next(x for x in doctor.ARTIFACTS if x['stock'])
        adb = FakeAdb({a['app_path']: a['stock']})
        self.assertEqual('clean', doctor.verdict(doctor.inventory(adb))[0])

    def test_the_env_file_is_part_of_the_inventory(self):
        names = {a['name'] for a in doctor.ARTIFACTS}
        self.assertIn('droiddeck-env', names)

    def test_inventory_reports_what_it_found(self):
        a = doctor.ARTIFACTS[0]
        adb = FakeAdb({a['app_path']: 'sha256:x'})
        entry = next(e for e in doctor.inventory(adb) if e['name'] == a['name'])
        self.assertTrue(entry['present']); self.assertEqual('sha256:x', entry['sha256'])
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_doctor.py' -v`
Expected: FAIL - modules missing.

- [ ] **Step 3: Write the implementation**

`tools/diagkit/device.py`:

```python
"""adb, with the project's own device resolver and nothing that cannot be faked in a test."""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

from adb_device import resolve_serial  # noqa: E402  (the shared resolver)


class AdbError(RuntimeError):
    pass


class Adb:
    def __init__(self, adb=None, serial=None):
        self.adb = adb or os.environ.get('ADB') or 'adb'
        self.serial = resolve_serial(self.adb, serial)

    def _run(self, *args, timeout=30):
        try:
            return subprocess.run([self.adb, '-s', self.serial, *args],
                                  capture_output=True, text=True, check=False, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise AdbError(str(exc)) from exc

    def shell(self, cmd, timeout=30):
        result = self._run('shell', cmd, timeout=timeout)
        if result.returncode != 0:
            raise AdbError(result.stderr.strip() or f'adb shell failed: {cmd}')
        return result.stdout.replace('\r\n', '\n')

    def ls(self, path):
        result = self._run('shell', f'ls -1 {path} 2>/dev/null || true')
        return [line for line in result.stdout.replace('\r', '').split('\n') if line.strip()]

    def sha256(self, path):
        out = self.shell(f'sha256sum {path} 2>/dev/null || echo MISSING').strip()
        if out.startswith('MISSING') or 'No such file' in out:
            return None
        return 'sha256:' + out.split()[0]

    def stat_size(self, path):
        out = self.shell(f'stat -c %s {path} 2>/dev/null || echo MISSING').strip()
        return None if out.startswith('MISSING') else int(out)
```

`tools/diagkit/doctor.py`:

```python
"""Is the device clean enough to measure on?

Session 33-34 ran after a rollback command that failed mid-USB-flap, and the session record cannot
say what was installed (§63). This is the check that makes "assume nothing" cheap: every artifact
the harness knows how to push, its app-side path, and the hash of the stock file it replaces.
Anything present that is not stock is a reason to refuse the run.
"""
from __future__ import annotations

GUEST = 'files/linuxfs'

ARTIFACTS = [
    {'name': 'crashcatch', 'app_path': f'{GUEST}/tmp/crashcatch.so', 'stock': None},
    {'name': 'mmwatch', 'app_path': f'{GUEST}/tmp/mmwatch.so', 'stock': None},
    {'name': 'symwatch', 'app_path': f'{GUEST}/tmp/symwatch.so', 'stock': None},
    {'name': 'cdp_probe.py', 'app_path': f'{GUEST}/tmp/cdp_probe.py', 'stock': None},
    {'name': 'x-inspect', 'app_path': f'{GUEST}/tmp/xinspect', 'stock': None},
    {'name': 'proc_watch', 'app_path': f'{GUEST}/tmp/proc_watch.sh', 'stock': None},
    {'name': 'gbm-shim', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/libgbm.so.1', 'stock': None},
    {'name': 'hybris-copy', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/libhybris.so', 'stock': None},
    {'name': 'q-copy', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/q.so', 'stock': None},
    {'name': 'droiddeck-env', 'app_path': '/sdcard/Download/droiddeck-env', 'stock': None},
]


def inventory(adb):
    entries = []
    for artifact in ARTIFACTS:
        digest = adb.sha256(artifact['app_path'])
        entries.append({
            'name': artifact['name'],
            'path': artifact['app_path'],
            'present': digest is not None,
            'sha256': digest,
            'stock': artifact['stock'],
        })
    return entries


def verdict(entries):
    reasons = []
    for entry in entries:
        if not entry['present']:
            continue
        if entry['stock'] and entry['sha256'] == entry['stock']:
            continue
        reasons.append(f"{entry['name']} is present at {entry['path']} ({entry['sha256']})")
    return ('dirty' if reasons else 'clean', reasons)
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_doctor.py' -v`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/device.py tools/diagkit/doctor.py tools/tests/test_diag_doctor.py
git commit -m "diag: doctor refuses a dirty device, because a failed rollback left sessions unreadable"
```

---

### Task 6: The run transaction

**Files:**
- Create: `tools/diagkit/runs.py`
- Test: `tools/tests/test_diag_runs.py`

**Interfaces:**
- Consumes: `manifest`, `doctor`, `Adb`.
- Produces: `Run` class: `__init__(root: Path, label: str, hypothesis: str, arm: dict, adb, commit: str, apk_version_code: int)`; `.begin() -> Path`; `.deploy(name: str, app_path: str, data: bytes, expected_sha: str) -> None`; `.deployed_maps(processes: dict[str, list[str]]) -> None`; `.finish(teardown_ok: bool, reasons: list[str]) -> dict`; `arm_differs(a: dict, b: dict) -> list[str]`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, tempfile, shutil, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

manifest = load('manifest'); runs = load('runs')


class FakeAdb:
    def __init__(self):
        self.files = {}
        self.written = []
    def push_bytes(self, path, data):
        self.written.append((path, data))
        self.files[path] = manifest.sha256_bytes(data)
    def sha256(self, path):
        return self.files.get(path)
    def rm(self, path):
        self.files.pop(path, None)


class RunTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.adb = FakeAdb()

    def make(self, arm=None):
        return runs.Run(self.tmp, 'display path', 'the layer path presents',
                        arm or {'name': 'icd', 'value': 'display-layer'},
                        self.adb, 'abc123', 40)

    def test_begin_creates_a_manifest_and_a_directory(self):
        run = self.make()
        d = run.begin()
        self.assertTrue((d / 'manifest.json').is_file())
        self.assertEqual('clean', manifest.trust(manifest.read_manifest(d)))

    def test_a_push_that_lands_with_the_expected_hash_is_clean(self):
        run = self.make(); run.begin()
        data = b'\x7fELF'
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', data, manifest.sha256_bytes(data))
        self.assertEqual('clean', manifest.trust(run.manifest))

    def test_a_push_that_lands_with_a_different_hash_taints(self):
        run = self.make(); run.begin()
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'\x7fELF', 'sha256:expectsomethingelse')
        self.assertEqual('tainted', manifest.trust(run.manifest))

    def test_a_process_loading_an_unexpected_inode_taints(self):
        run = self.make(); run.begin()
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'x', manifest.sha256_bytes(b'x'))
        run.deployed_maps({'steam': ['sha256:' + 'a' * 64]})
        self.assertEqual('tainted', manifest.trust(run.manifest))

    def test_a_process_loading_the_deployed_inode_does_not_taint(self):
        run = self.make(); run.begin()
        sha = manifest.sha256_bytes(b'x')
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'x', sha)
        run.deployed_maps({'steam': [sha]})
        self.assertEqual('clean', manifest.trust(run.manifest))

    def test_a_failed_teardown_taints_and_is_recorded(self):
        run = self.make(); run.begin()
        m = run.finish(teardown_ok=False, reasons=['shim still present'])
        self.assertEqual('tainted', manifest.trust(m))
        self.assertEqual('failed', m['teardown'])

    def test_arm_differs_reports_each_differing_field(self):
        self.assertEqual(['value'], runs.arm_differs(
            {'name': 'icd', 'value': 'A', 'shim': 'nulldev'},
            {'name': 'icd', 'value': 'B', 'shim': 'nulldev'}))

    def test_arm_differs_is_empty_for_one_variable(self):
        self.assertEqual([], runs.arm_differs({'value': 'A'}, {'value': 'A'}))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_runs.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Write the implementation**

```python
"""A run is a transaction: begin, deploy, verify, finish, and say whether the device is restored.

The failure this prevents is the recorded one (§63): a rollback ran while the device flaked off
USB, failed, and two sessions executed with the previous configuration still installed while the
notes said otherwise. Here a teardown that did not verify is not an error to mention - it is a
taint that makes the run's evidence ineligible as sole proof.
"""
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path

try:
    from . import manifest
except ImportError:  # loaded by file path in tests
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        'manifest', Path(__file__).with_name('manifest.py'))
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)


def arm_differs(a, b):
    keys = sorted(set(a) | set(b))
    return [k for k in keys if a.get(k) != b.get(k)]


class Run:
    def __init__(self, root, label, hypothesis, arm, adb, commit, apk_version_code):
        self.root = Path(root)
        self.label = label
        self.hypothesis = hypothesis
        self.arm = arm
        self.adb = adb
        self.commit = commit
        self.apk_version_code = apk_version_code
        self.manifest = None
        self.dir = None
        self._deployed = {}

    def begin(self):
        stamp = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ')
        slug = ''.join(c if c.isalnum() or c in '-_' else '-' for c in self.label)[:48].strip('-')
        self.dir = self.root / f'{stamp}-{slug}'
        self.dir.mkdir(parents=True, exist_ok=True)
        self.manifest = manifest.new_manifest(
            self.label, self.hypothesis, self.arm, self.commit, self.apk_version_code)
        self.manifest['startedAt'] = stamp
        manifest.write_manifest(self.dir, self.manifest)
        return self.dir

    def deploy(self, name, app_path, data, expected_sha):
        self.adb.push_bytes(app_path, data)
        pushed = self.adb.sha256(app_path)
        manifest.stamp_deploy(self.manifest, app_path, expected_sha, pushed)
        if pushed:
            self._deployed[name] = pushed
        manifest.write_manifest(self.dir, self.manifest)
        return pushed

    def deployed_maps(self, processes):
        """processes: {process: [sha256 of each instrument mapped, in order]}."""
        for process, found in processes.items():
            for digest in found:
                if digest not in self._deployed.values():
                    manifest.mark_tainted(
                        self.manifest, f'{process} maps {digest}, which this run did not deploy')
        self.manifest['maps'] = processes
        manifest.write_manifest(self.dir, self.manifest)

    def finish(self, teardown_ok, reasons):
        self.manifest['teardown'] = 'ok' if teardown_ok else 'failed'
        for reason in reasons:
            manifest.mark_tainted(self.manifest, f'teardown: {reason}')
        self.manifest['finishedAt'] = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ')
        manifest.write_manifest(self.dir, self.manifest)
        return self.manifest
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_runs.py' -v`
Expected: PASS (8 tests).

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/runs.py tools/tests/test_diag_runs.py
git commit -m "diag: a run is a verified transaction, and a failed teardown taints its evidence"
```

---

### Task 7: The CLI

**Files:**
- Create: `tools/diagkit/cli.py`, `tools/diag`
- Test: (covered by the existing suites; add one smoke test in `tools/tests/test_diag_cli.py`)

**Interfaces:**
- Produces: `build_parser() -> argparse.ArgumentParser`; `main(argv=None) -> int`; exit codes `0` ok, `2` bad usage, `3` device error, `4` refused (dirty device / arms differ), `5` timeout, `6` file error.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, io, unittest
from contextlib import redirect_stdout
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cli', TOOLS / 'diagkit/cli.py')
cli = importlib.util.module_from_spec(spec); spec.loader.exec_module(cli)


class CliTest(unittest.TestCase):
    def test_every_documented_command_exists(self):
        parser = cli.build_parser()
        actions = [a for a in parser._subparsers._group_actions[0].choices]
        for name in ('doctor', 'run', 'prior', 'analyze', 'compare', 'frame', 'export'):
            self.assertIn(name, actions)

    def test_run_requires_a_label_and_an_arm(self):
        parser = cli.build_parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(['run', '--label', 'x'])
        ns = parser.parse_args(['run', '--label', 'x', '--arm', 'icd=A', '--hypothesis', 'h'])
        self.assertEqual({'icd': 'A'}, ns.arm_dict)

    def test_usage_errors_exit_2(self):
        self.assertEqual(2, cli.main(['nonesuch']))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_cli.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Write the implementation**

```python
"""The front door. Every command prints JSON on stdout and a human line on stderr."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def _arm(pairs):
    out = {}
    for pair in pairs or []:
        if '=' not in pair:
            raise argparse.ArgumentTypeError(f'--arm wants name=value, got {pair!r}')
        k, v = pair.split('=', 1)
        out[k] = v
    return out


def build_parser():
    p = argparse.ArgumentParser(prog='diag', description='DroidDeck diagnostic harness')
    p.add_argument('--adb')
    p.add_argument('--serial')
    sub = p.add_subparsers(dest='command', required=True)

    sub.add_parser('doctor', help='is the device clean enough to measure on')

    r = sub.add_parser('run', help='run one labelled experiment')
    r.add_argument('--label', required=True)
    r.add_argument('--hypothesis', required=True)
    r.add_argument('--arm', action='append', type=str, required=True,
                   help='name=value for the ONE thing this run varies')
    r.add_argument('--root', default='runs')
    r.set_defaults(arm_dict=None)

    pr = sub.add_parser('prior', help='search every existing run before forming a hypothesis')
    pr.add_argument('question')
    pr.add_argument('--roots', nargs='*', default=[])

    a = sub.add_parser('analyze', help='timeline, symbolization and cited findings')
    a.add_argument('run')
    a.add_argument('--symbolize', action='store_true')

    c = sub.add_parser('compare', help='diff two runs, refusing arms that vary more than one thing')
    c.add_argument('a'); c.add_argument('b')

    f = sub.add_parser('frame', help='digest a frame file (raw RGBA) or the live panel')
    f.add_argument('--raw', help='path to a raw RGBA frame with pitch=width*4')
    f.add_argument('--width', type=int); f.add_argument('--height', type=int)
    f.add_argument('--panel', action='store_true', help='screencap the device (explicit request only)')
    f.add_argument('--ascii', action='store_true')

    e = sub.add_parser('export', help='stage a text-only bundle of a run')
    e.add_argument('run'); e.add_argument('dest', nargs='?')
    return p


def main(argv=None):
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code not in (0, None) else 0
    if args.command == 'run':
        args.arm_dict = _arm(args.arm)
    try:
        return _dispatch(args)
    except Exception as exc:  # the CLI's contract is an exit code, not a traceback
        print(json.dumps({'ok': False, 'error': str(exc)}))
        print(f'diag: {exc}', file=sys.stderr)
        return getattr(exc, 'exit_code', 2)


def _dispatch(args):
    from . import doctor as doctor_mod
    if args.command == 'doctor':
        adb = _adb(args)
        inv = doctor_mod.inventory(adb)
        state, reasons = doctor_mod.verdict(inv)
        print(json.dumps({'ok': state == 'clean', 'state': state, 'reasons': reasons, 'inventory': inv}))
        return 0 if state == 'clean' else 4
    raise SystemExit(2)


def _adb(args):
    from .device import Adb
    return Adb(args.adb, args.serial)
```

`tools/diag`:

```python
#!/usr/bin/env python3
"""Run the diagnostic harness: `python3 tools/diag <command>`."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from diagkit.cli import main  # noqa: E402

if __name__ == '__main__':
    raise SystemExit(main())
```

Make it executable: `git update-index --chmod=+x tools/diag` (or `chmod +x` before adding).

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_cli.py' -v`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
chmod +x tools/diag
git add tools/diagkit/cli.py tools/diag tools/tests/test_diag_cli.py
git commit -m "diag: the CLI, with exit codes a script can branch on"
```

---

### Task 8: The guest calibrator

**Files:**
- Create: `docs/development/pvr-re/rig/calibrate.c`
- Modify: `docs/development/pvr-re/rig/build.sh` (add `calibrate` to the loop)
- Test: `tools/tests/test_diag_native.py` (new; source assertions)

**Interfaces:**
- The binary, run as `calibrate <mode>`, where mode is one of `mmap`, `dlopen`, `crash`, `mark`.
  - `mmap` prints a line containing `CAL-MMAP <addr> <len>`.
  - `dlopen` prints `CAL-DLOPEN <path> <handle>`.
  - `crash` faults at a fixed address and prints `CAL-CRASH <addr>` before faulting; crashcatch must report that address.
  - `mark <label>` prints `MARK-<label>`; used to prove the log path and the correlation clock.

- [ ] **Step 1: Write the failing test**

```python
import unittest
from pathlib import Path

RIG = Path(__file__).resolve().parents[2] / 'docs/development/pvr-re/rig'


class CalibratorSourceTest(unittest.TestCase):
    def setUp(self):
        self.src = (RIG / 'calibrate.c').read_text(encoding='utf-8')

    def test_it_supports_the_four_modes(self):
        for mode in ('mmap', 'dlopen', 'crash', 'mark'):
            self.assertIn('"%s"' % mode, self.src)

    def test_the_crash_mode_uses_a_fixed_address_it_also_prints(self):
        self.assertIn('CAL-CRASH', self.src)
        self.assertIn('0xdeadbe0', self.src.lower())

    def test_the_build_script_builds_it(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('calibrate', build)

    def test_it_does_not_require_glibc_newer_than_the_runtime_floor(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('aarch64-linux-gnu.2.31', build)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_native.py' -v`
Expected: FAIL - `calibrate.c` missing.

- [ ] **Step 3: Write the implementation**

```c
/* calibrate - the known answer each guest instrument must reproduce before it is believed.
 *
 * mmwatch must show the mmap below with this caller; symwatch must show the dlopen; crashcatch
 * must report the address this prints; the marker proves the log route and the clock. A digest
 * producer needs none of this - its pattern check runs in the compositor and on the host.
 *
 * usage: calibrate mmap | dlopen | crash | mark <label>
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CRASH_ADDR ((volatile unsigned char *)0xdeadbe0UL)

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "mmap")) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { perror("mmap"); return 1; }
        printf("CAL-MMAP %p %d\n", p, 4096);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(mode, "dlopen")) {
        void *h = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
        printf("CAL-DLOPEN libc.so.6 %p\n", h);
        fflush(stdout);
        return h ? 0 : 1;
    }
    if (!strcmp(mode, "mark")) {
        const char *label = argc > 2 ? argv[2] : "unnamed";
        printf("MARK-%s\n", label);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(mode, "crash")) {
        /* A fixed, computed-looking address in the same shape as the real faults (§63 si_addr
         * ended fc028 in every crash). Printing it first means a report can be matched to this
         * run rather than to a previous one. */
        printf("CAL-CRASH %p\n", (void *)CRASH_ADDR);
        fflush(stdout);
        signal(SIGSEGV, SIG_DFL);
        *CRASH_ADDR = 1;
        return 0;
    }
    fprintf(stderr, "usage: %s mmap|dlopen|crash|mark <label>\n", argv[0]);
    return 2;
}
```

Modify `docs/development/pvr-re/rig/build.sh`: change the loop line

```sh
for f in mmwatch symwatch crashcatch; do
```

to

```sh
for f in mmwatch symwatch crashcatch calibrate; do
```

and keep the same flags. `calibrate` is an executable, not a shared object, so append after the loop:

```sh
"$ZIG" cc -target aarch64-linux-gnu.2.31 -O1 -o "$D/dist/calibrate" "$D/calibrate.c"
echo "built calibrate"
```

- [ ] **Step 4: Build it in WSL and run the tests**

Run (WSL): `wsl -e bash -lc 'cd /mnt/d/GUH/DroidDeck && bash docs/development/pvr-re/rig/build.sh'`
Expected: `built calibrate`, and `dist/calibrate` exists.

Run (host): `python3 -m unittest discover -s tools/tests -p 'test_diag_native.py' -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add docs/development/pvr-re/rig/calibrate.c docs/development/pvr-re/rig/build.sh tools/tests/test_diag_native.py
git commit -m "diag: a guest calibrator, so mmwatch/symwatch/crashcatch prove themselves each run"
```

---

## Phase 2 - Frames

### Task 9: The C digest core and Python/C parity

**Files:**
- Create: `app/src/main/cpp/waylandcomp/src/frame_digest.h`, `frame_digest.c`, `frame_digest_selftest.c`
- Test: `tools/tests/test_diag_frames_parity.py`

**Interfaces:**
- Produces (C): `struct fd_digest { int w, h, zero_rows, zero_run_count, run_count; int bands[FD_BANDS][3]; int zero_runs[FD_MAX_RUNS][2]; int rl[FD_RL]; int grid[FD_GRID][FD_GRID]; char hash_hex[65]; };` and `int fd_digest_rgba(const unsigned char *px, int w, int h, int pitch, struct fd_digest *out);` and `int fd_ascii(const unsigned char *px, int w, int h, int pitch, int cols, int rows, char *out, int out_len);`
- The self-test driver emits the same JSON field names as `frames.digest_rgba`, so the parity test compares parsed JSON.

- [ ] **Step 1: Write the failing parity test**

```python
import importlib.util, json, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'app/src/main/cpp/waylandcomp/src'
TOOLS = ROOT / 'tools'
spec = importlib.util.spec_from_file_location('frames', TOOLS / 'diagkit/frames.py')
frames = importlib.util.module_from_spec(spec); spec.loader.exec_module(frames)

COMPILER = shutil.which('gcc') or shutil.which('cc') or shutil.which('clang')


@unittest.skipUnless(COMPILER, 'no host C compiler; the parity check needs one')
class ParityTest(unittest.TestCase):
    def build(self, tmp):
        exe = Path(tmp) / ('fdselftest.exe' if __import__('os').name == 'nt' else 'fdselftest')
        subprocess.run([COMPILER, '-std=c11', '-O1', '-o', str(exe),
                        str(SRC / 'frame_digest.c'), str(SRC / 'frame_digest_selftest.c')],
                       check=True, capture_output=True)
        return exe

    def test_python_and_c_agree_on_a_padded_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build(tmp)
            w, h, pad = 37, 23, 20
            px = frames.pattern_rgba(w, h)
            rows = [px[y * w * 4:(y + 1) * w * 4] + b'\x11\x22\x33\x44' * (pad // 4) for y in range(h)]
            data = b''.join(rows)
            raw = Path(tmp) / 'frame.raw'; raw.write_bytes(data)
            out = subprocess.run([str(exe), str(raw), str(w), str(h), str(w * 4 + pad)],
                                 check=True, capture_output=True, text=True).stdout
            self.assertEqual(frames.digest_rgba(data, w, h, pitch=w * 4 + pad), json.loads(out))

    def test_python_and_c_agree_on_an_ascii_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build(tmp)
            w, h = 64, 40
            data = frames.pattern_rgba(w, h)
            raw = Path(tmp) / 'frame.raw'; raw.write_bytes(data)
            out = subprocess.run([str(exe), str(raw), str(w), str(h), str(w * 4), '--ascii'],
                                 check=True, capture_output=True, text=True).stdout
            self.assertEqual(frames.digest_rgba(data, w, h, want_ascii=True), json.loads(out))

    def test_the_c_core_refuses_a_short_buffer(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = self.build(tmp)
            raw = Path(tmp) / 'short.raw'; raw.write_bytes(b'\x00' * 16)
            result = subprocess.run([str(exe), str(raw), '8', '8', '32'], capture_output=True)
            self.assertNotEqual(0, result.returncode)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_frames_parity.py' -v`
Expected: FAIL - sources missing.

- [ ] **Step 3: Write the C core**

`frame_digest.h`:

```c
/* A frame as text, in the compositor. Dependency-free: no Vulkan, no logcat, no allocation.
 * Kept in lockstep with tools/diagkit/frames.py by test_diag_frames_parity.py - if the two ever
 * disagree, two instruments cannot be compared and the comparison is worthless. */
#pragma once
#include <stddef.h>

#define FD_BANDS 32
#define FD_GRID 16
#define FD_RL 13
#define FD_MAX_RUNS 256
#define FD_RAMP " .:-=+*#%@"

struct fd_digest {
    int w, h;
    int zero_rows;
    int zero_run_count;
    int bands[FD_BANDS][3];        /* mean, spread, zero rows */
    int zero_runs[FD_MAX_RUNS][2]; /* start, length */
    int rl[FD_RL];
    int grid[FD_GRID][FD_GRID];
    char hash_hex[65];
};

/* Returns 0 on success, -1 if the buffer is too short for w x h at pitch. */
int fd_digest_rgba(const unsigned char *px, int w, int h, int pitch, struct fd_digest *out);

/* cols x rows mean-luminance map, as characters, newline-separated. Returns the length. */
int fd_ascii(const unsigned char *px, int w, int h, int pitch, int cols, int rows,
             char *out, int out_len);
```

`frame_digest.c`:

```c
#include "frame_digest.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SHA-256, written out rather than linked: the compositor may not depend on libcrypto, and the
 * Python side uses hashlib. Both must hash the same bytes in the same order - each row up to
 * width*4, skipping the pitch gap - or the parity test fails. */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

struct sha { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t used; };

static void sha_init(struct sha *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->used = 0;
}

static void sha_block(struct sha *s, const unsigned char *p) {
    uint32_t w[64], a,b,c,d,e,f,g,h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha_update(struct sha *s, const unsigned char *p, size_t n) {
    s->len += n;
    while (n) {
        size_t take = 64 - s->used; if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static void sha_final(struct sha *s, char hex[65]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80; sha_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->used != 56) sha_update(s, &zero, 1);
    for (int i = 7; i >= 0; i--) { unsigned char b = (unsigned char)(bits >> (i*8)); sha_update(s, &b, 1); }
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
            unsigned char nib = (unsigned char)((s->h[i] >> ((3 - j) * 8 + 4)) & 0xf);
            unsigned char low = (unsigned char)((s->h[i] >> ((3 - j) * 8)) & 0xf);
            hex[i*8 + j*2] = hx[nib]; hex[i*8 + j*2 + 1] = hx[low];
        }
    hex[64] = 0;
}

static int luma(const unsigned char *p) { return (77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8; }

static int bucket(int n) {
    static const int caps[FD_RL] = {1,2,4,8,16,32,64,128,256,512,1024,2048,4096};
    for (int i = 0; i < FD_RL; i++) if (n <= caps[i]) return i;
    return FD_RL - 1;
}

int fd_digest_rgba(const unsigned char *px, int w, int h, int pitch, struct fd_digest *out) {
    if (!px || !out || w <= 0 || h <= 0 || pitch < w * 4) return -1;
    memset(out, 0, sizeof *out);
    out->w = w; out->h = h;

    int means[h]; int zeros[h];
    struct sha s; sha_init(&s);
    for (int y = 0; y < h; y++) {
        const unsigned char *row = px + (size_t)y * pitch;
        long sr = 0, sg = 0, sb = 0; int z = 1;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = row + x * 4;
            sr += p[0]; sg += p[1]; sb += p[2];
            if (p[0] || p[1] || p[2] || p[3]) z = 0;
        }
        means[y] = (int)((77 * sr + 150 * sg + 29 * sb) / (256L * w));
        zeros[y] = z;
        sha_update(&s, row, (size_t)w * 4);
    }
    sha_final(&s, out->hash_hex);

    for (int b = 0; b < FD_BANDS; b++) {
        int y0 = b * h / FD_BANDS, y1 = (b + 1) * h / FD_BANDS;
        if (y1 <= y0) y1 = y0 + 1;
        long total = 0; int mn = 255, mx = 0, zc = 0;
        for (int y = y0; y < y1 && y < h; y++) {
            total += means[y]; if (means[y] < mn) mn = means[y]; if (means[y] > mx) mx = means[y];
            zc += zeros[y];
        }
        int n = (y1 > h ? h : y1) - y0; if (n < 1) n = 1;
        out->bands[b][0] = (int)(total / n);
        out->bands[b][1] = mx - mn;
        out->bands[b][2] = zc;
    }

    for (int y = 0; y < h; y++) {
        out->zero_rows += zeros[y];
        if (zeros[y]) {
            if (out->zero_run_count && out->zero_runs[out->zero_run_count-1][0] +
                out->zero_runs[out->zero_run_count-1][1] == y)
                out->zero_runs[out->zero_run_count-1][1]++;
            else if (out->zero_run_count < FD_MAX_RUNS) {
                out->zero_runs[out->zero_run_count][0] = y;
                out->zero_runs[out->zero_run_count][1] = 1;
                out->zero_run_count++;
            }
        }
    }

    /* The run-length profile of the first row, matching frames._run_lengths. */
    if (h > 0) {
        const unsigned char *row = px; int run = 0; const unsigned char *prev = NULL;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = row + x * 4;
            if (prev && !memcmp(prev, p, 4)) { run++; continue; }
            if (prev) out->rl[bucket(run)]++;
            prev = p; run = 1;
        }
        if (prev) out->rl[bucket(run)]++;
    }

    for (int r = 0; r < FD_GRID; r++) {
        int y0 = r * h / FD_GRID, y1 = (r + 1) * h / FD_GRID;
        for (int c = 0; c < FD_GRID; c++) {
            int x0 = c * w / FD_GRID, x1 = (c + 1) * w / FD_GRID;
            long total = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char *p = px + (size_t)y * pitch;
                for (int x = x0; x < x1; x++) {
                    const unsigned char *q = p + x * 4;
                    total += 77 * q[0] + 150 * q[1] + 29 * q[2];
                }
            }
            long n = (long)(y1 - y0) * (x1 - x0); if (n < 1) n = 1;
            out->grid[r][c] = (int)(total / (256L * n));
        }
    }
    return 0;
}

int fd_ascii(const unsigned char *px, int w, int h, int pitch, int cols, int rows,
             char *out, int out_len) {
    const char *ramp = FD_RAMP; int ramp_n = 10, n = 0;
    for (int r = 0; r < rows; r++) {
        int y0 = r * h / rows, y1 = (r + 1) * h / rows;
        for (int c = 0; c < cols; c++) {
            int x0 = c * w / cols, x1 = (c + 1) * w / cols;
            long total = 0;
            for (int y = y0; y < y1; y++) {
                const unsigned char *p = px + (size_t)y * pitch;
                for (int x = x0; x < x1; x++) {
                    const unsigned char *q = p + x * 4;
                    total += 77 * q[0] + 150 * q[1] + 29 * q[2];
                }
            }
            long cells = (long)(y1 - y0) * (x1 - x0); if (cells < 1) cells = 1;
            int v = (int)(total / (256L * cells));
            int idx = v * ramp_n / 256; if (idx < 0) idx = 0; if (idx >= ramp_n) idx = ramp_n - 1;
            if (n + 2 >= out_len) return -1;
            out[n++] = ramp[idx];
        }
        if (n + 2 >= out_len) return -1;
        out[n++] = '\n';
    }
    out[n ? n - 1 : 0] = 0;   /* drop the trailing newline, as Python's join does */
    return n - 1;
}
```

`frame_digest_selftest.c`:

```c
/* Host self-test: read a raw RGBA file and print the digest as JSON, field for field the same as
 * tools/diagkit/frames.digest_rgba. The parity test is what keeps the two implementations honest.
 * usage: fdselftest <file.raw> <w> <h> <pitch> [--ascii] */
#include "frame_digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s <raw> <w> <h> <pitch> [--ascii]\n", argv[0]); return 2; }
    int w = atoi(argv[2]), h = atoi(argv[3]), pitch = atoi(argv[4]);
    int want_ascii = argc > 5 && !strcmp(argv[5], "--ascii");
    size_t need = (size_t)pitch * (h - 1) + (size_t)w * 4;
    unsigned char *px = malloc(need + 1);
    if (!px) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    size_t got = fread(px, 1, need, f);
    fclose(f);
    if (got < need) { fprintf(stderr, "short buffer: %zu of %zu\n", got, need); return 1; }

    struct fd_digest d;
    if (fd_digest_rgba(px, w, h, pitch, &d) != 0) { fprintf(stderr, "digest failed\n"); return 1; }

    printf("{\"w\":%d,\"h\":%d,\"bands\":[", d.w, d.h);
    for (int i = 0; i < FD_BANDS; i++)
        printf("%s[%d,%d,%d]", i ? "," : "", d.bands[i][0], d.bands[i][1], d.bands[i][2]);
    printf("],\"zero_rows\":%d,\"zero_runs\":[", d.zero_rows);
    for (int i = 0; i < d.zero_run_count; i++)
        printf("%s[%d,%d]", i ? "," : "", d.zero_runs[i][0], d.zero_runs[i][1]);
    printf("],\"rl\":[");
    for (int i = 0; i < FD_RL; i++) printf("%s%d", i ? "," : "", d.rl[i]);
    printf("],\"grid\":[");
    for (int r = 0; r < FD_GRID; r++) {
        printf("%s[", r ? "," : "");
        for (int c = 0; c < FD_GRID; c++) printf("%s%d", c ? "," : "", d.grid[r][c]);
        printf("]");
    }
    printf("],\"hash\":\"sha256:%s\"", d.hash_hex);
    if (want_ascii) {
        char buf[8192];
        if (fd_ascii(px, w, h, pitch, 48, 20, buf, sizeof buf) < 0) return 1;
        printf(",\"ascii\":\"");
        for (const char *p = buf; *p; p++) {
            if (*p == '\n') printf("\\n");
            else if (*p == '"' || *p == '\\') printf("\\%c", *p);
            else putchar(*p);
        }
        printf("\"");
    }
    printf("}\n");
    free(px);
    return 0;
}
```

- [ ] **Step 4: Run the parity tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_frames_parity.py' -v`
Expected: PASS (3 tests). If they fail, the mismatch is real - fix the side that deviates from the spec in Task 2, do not adjust the test.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/cpp/waylandcomp/src/frame_digest.h \
        app/src/main/cpp/waylandcomp/src/frame_digest.c \
        app/src/main/cpp/waylandcomp/src/frame_digest_selftest.c \
        tools/tests/test_diag_frames_parity.py
git commit -m "diag: the frame digest in C, held to byte-identical output with Python"
```

---

### Task 10: The compositor emits digests, adaptively

**Files:**
- Modify: `app/src/main/cpp/waylandcomp/CMakeLists.txt`, `src/vk_present.c`, `src/vk_present.h`
- Test: extend `tools/tests/test_diag_native.py`

**Interfaces:**
- Produces: `int vkp_frame_digest(struct vkp_image *src, int level);` where `level` is 0 off, 1 numeric, 2 numeric+ASCII. Emits one line per sample through `droiddeck_log("fd", ...)`.
- Produces (internal): `fd_should_sample(long frame_index, int changed, long mono_ms) -> int`.

- [ ] **Step 1: Write the failing source assertions**

Append to `tools/tests/test_diag_native.py`:

```python
class CompositorDigestTest(unittest.TestCase):
    def setUp(self):
        self.src = (Path(__file__).resolve().parents[2]
                    / 'app/src/main/cpp/waylandcomp/src/vk_present.c').read_text(encoding='utf-8')
        self.cmake = (Path(__file__).resolve().parents[2]
                      / 'app/src/main/cpp/waylandcomp/CMakeLists.txt').read_text(encoding='utf-8')

    def test_the_digest_is_compiled_in(self):
        self.assertIn('src/frame_digest.c', self.cmake)

    def test_the_staging_image_is_sized_from_the_source_extent(self):
        """The defect that produced §65-68 was a fixed-size staging buffer. This forbids one."""
        body = self.src.split('vkp_frame_digest', 1)[1]
        self.assertIn('src->w', body)
        self.assertIn('src->h', body)
        self.assertIn('GetImageSubresourceLayout', body)

    def test_the_readback_does_not_reuse_the_cursor_helper(self):
        body = self.src.split('vkp_frame_digest', 1)[1].split('vkp_image_readback', 1)[0]
        self.assertNotIn('vkp_image_readback', body)

    def test_sampling_is_adaptive_not_fixed(self):
        self.assertIn('fd_should_sample', self.src)

    def test_the_digest_is_off_by_default(self):
        self.assertIn('BL_FRAME_DIGEST', self.src)
        self.assertRegex(self.src, r'getenv\("BL_FRAME_DIGEST"\)')
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_native.py' -v`
Expected: FAIL on the compositor tests.

- [ ] **Step 3: Implement it**

Append to `vk_present.c` (and declare in `vk_present.h`):

```c
/* ---------------------------------------------------------------- frame digest
 *
 * Evidence about what is on screen, as text. Gate: BL_FRAME_DIGEST=1 (numeric) or 2 (with the
 * ASCII map). Cadence is adaptive: dense for the first frames, sparse afterwards, and always on a
 * frame whose character changed - a transient fault is what a fixed interval misses.
 *
 * The staging image is built from the SOURCE's extent and its real subresource layout. That is
 * the whole lesson of state doc §65-68: a fixed-size helper answered a question it was never built
 * for, and three findings had to be retracted.
 */
static struct vkp_image *g_fd;             /* the digest staging image, sized for one shape */
static long g_fd_frame, g_fd_last_mono, g_fd_last_hash;
static int g_fd_level = -1;

static int fd_level(void) {
    if (g_fd_level < 0) {
        const char *v = getenv("BL_FRAME_DIGEST");
        g_fd_level = (v && *v) ? atoi(v) : 0;
    }
    return g_fd_level;
}

/* Dense for the first 120 frames, then every 30th, plus any frame whose bands moved. */
int fd_should_sample(long frame, int changed, long mono_ms) {
    if (frame < 120) return 1;
    if (changed) return 1;
    if (mono_ms - g_fd_last_mono > 5000) return 1;
    return (frame % 30) == 0;
}

int vkp_frame_digest(struct vkp_image *src, int level) {
    if (level <= 0 || !src || src->w <= 0 || src->h <= 0) return -1;
    if (g_dev_state == -2 || dev_init() != 0) return -1;
    if (g_fd && (g_fd->w != src->w || g_fd->h != src->h)) { vkp_image_destroy(g_fd); g_fd = NULL; }
    if (!g_fd) {
        struct vkp_image *img = calloc(1, sizeof(*img));
        if (!img) return -1;
        img->w = src->w; img->h = src->h;
        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = {(uint32_t)src->w, (uint32_t)src->h, 1},
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (g_vk.CreateImage(g_dev, &ici, NULL, &img->image) != VK_SUCCESS) { free(img); return -1; }
        VkMemoryRequirements req;
        g_vk.GetImageMemoryRequirements(g_dev, img->image, &req);
        int idx = memory_type(req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (idx < 0) { g_vk.DestroyImage(g_dev, img->image, NULL); free(img); return -1; }
        VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                    .allocationSize = req.size, .memoryTypeIndex = (uint32_t)idx};
        if (g_vk.AllocateMemory(g_dev, &mai, NULL, &img->mem) != VK_SUCCESS) {
            g_vk.DestroyImage(g_dev, img->image, NULL); free(img); return -1; }
        g_vk.BindImageMemory(g_dev, img->image, img->mem, 0);
        VkImageSubresource subr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout lay;
        g_vk.GetImageSubresourceLayout(g_dev, img->image, &subr, &lay);
        img->offset = lay.offset;
        img->row_pitch = lay.rowPitch;
        if (g_vk.MapMemory(g_dev, img->mem, 0, req.size, 0, &img->map) != VK_SUCCESS) {
            vkp_image_destroy(img); return -1; }
        g_fd = img;
    }
    /* Copy the frame in, then digest the mapped bytes at the layout the driver reported. */
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cmd = g_cmds[0];
    g_vk.ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    g_vk.BeginCommandBuffer(cmd, &bi);
    VkImageMemoryBarrier pre[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = src->image, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = g_fd->image, .subresourceRange = range, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT}};
    g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, 0, NULL, 0, NULL, 2, pre);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {src->w, src->h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {src->w, src->h, 1}}};
    g_vk.CmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      g_fd->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    VkImageMemoryBarrier post[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_fd->image,
         .subresourceRange = range, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_HOST_READ_BIT},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = src->image,
         .subresourceRange = range, .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT}};
    g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            0, 0, NULL, 0, NULL, 2, post);
    g_vk.EndCommandBuffer(cmd);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                       .pCommandBuffers = &cmd};
    g_vk.ResetFences(g_dev, 1, &g_fence);
    if (g_vk.QueueSubmit(g_queue, 1, &si, g_fence) != VK_SUCCESS) return -1;
    if (timed_wait(g_fence, 1000000000ULL) != VK_SUCCESS) return -1;

    struct fd_digest d;
    if (fd_digest_rgba((const unsigned char *)g_fd->map + g_fd->offset,
                       src->w, src->h, (int)g_fd->row_pitch, &d) != 0) return -1;

    int changed = (long)strtoul(d.hash_hex, NULL, 16) != g_fd_last_hash && g_fd_frame > 0;
    g_fd_last_hash = (long)strtoul(d.hash_hex, NULL, 16);

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long mono = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
    if (!fd_should_sample(g_fd_frame, changed, mono)) { g_fd_frame++; return 0; }
    g_fd_last_mono = mono;

    droiddeck_log("fd", "t=%ld.%03ld %dx%d pitch=%llu zero_rows=%d hash=%s",
                  ts.tv_sec, ts.tv_nsec / 1000000L, d.w, d.h,
                  (unsigned long long)g_fd->row_pitch, d.zero_rows, d.hash_hex);
    if (level >= 2) {
        char map[4096];
        if (fd_ascii((const unsigned char *)g_fd->map + g_fd->offset, src->w, src->h,
                     (int)g_fd->row_pitch, 48, 20, map, sizeof map) > 0) {
            const char *p = map;
            while (*p) { const char *nl = strchr(p, '\n'); int n = nl ? (int)(nl - p) : (int)strlen(p);
                         droiddeck_log("fd", "map|%.*s|", n, p); p = nl ? nl + 1 : p + n; }
        }
    }
    g_fd_frame++;
    return 0;
}
```

Declare in `vk_present.h`:

```c
/* Evidence about a composed frame, as text. level 0 off, 1 numeric, 2 numeric + ASCII map. */
int vkp_frame_digest(struct vkp_image *src, int level);
int fd_should_sample(long frame, int changed, long mono_ms);
```

Add `src/frame_digest.c` to the `add_library(droiddeckwayland SHARED ...)` source list in `CMakeLists.txt`, and add `#include "../frame_digest.h"` at the top of `vk_present.c` next to the other includes.

**Call site:** in the display-layer path, immediately after the scene is composed and before it is handed to the layer, add:

```c
    if (fd_level() > 0) vkp_frame_digest(g_scene, fd_level());
```

(Use the scene image variable already in that function; do not add a second compose.)

- [ ] **Step 4: Build and run the tests**

Run: `JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew assembleDebug --console=plain` (native compiles as part of it).
Expected: BUILD SUCCESSFUL.

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_native.py' -v`
Expected: PASS (all).

- [ ] **Step 5: Commit**

```bash
git add app/src/main/cpp/waylandcomp/CMakeLists.txt app/src/main/cpp/waylandcomp/src/vk_present.c \
        app/src/main/cpp/waylandcomp/src/vk_present.h tools/tests/test_diag_native.py
git commit -m "compositor: digest each composed frame at its true extent, adaptively, behind BL_FRAME_DIGEST"
```

---

### Task 11: The panel producer and `diag frame`

**Files:**
- Modify: `tools/diagkit/cli.py` (the `frame` branch), `tools/diagkit/device.py` (add `screencap_raw`)
- Test: extend `tools/tests/test_diag_cli.py`

**Interfaces:**
- Produces: `Adb.screencap_raw() -> bytes`; CLI `diag frame --panel --ascii` prints a digest JSON; `--raw PATH --width W --height H [--pitch N]` digests a file.

- [ ] **Step 1: Write the failing test**

```python
class FrameCommandTest(unittest.TestCase):
    def test_a_raw_file_is_digested(self):
        import json, struct, tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'f.raw'
            w, h = 4, 4
            p.write_bytes(bytes([10, 20, 30, 255]) * (w * h))
            import contextlib, io
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = cli.main(['frame', '--raw', str(p), '--width', str(w), '--height', str(h)])
            self.assertEqual(0, code)
            d = json.loads(buf.getvalue())
            self.assertEqual((w, h), (d['w'], d['h']))
            self.assertEqual(0, d['zero_rows'])
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_cli.py' -v`
Expected: FAIL - `frame` not dispatched.

- [ ] **Step 3: Implement**

Add to `device.py`:

```python
    def screencap_raw(self):
        """Raw RGBA from SurfaceFlinger. The caller decodes it and must not persist it."""
        result = self._run('exec-out', 'screencap', timeout=60)
        if result.returncode != 0 or len(result.stdout) < 12:
            raise AdbError('screencap failed')
        return result.stdout
```

Note: `_run` uses `text=True`; for a binary payload add a `binary` flag:

```python
    def _run(self, *args, timeout=30, binary=False):
        try:
            return subprocess.run([self.adb, '-s', self.serial, *args],
                                  capture_output=True, text=not binary,
                                  check=False, timeout=timeout)
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise AdbError(str(exc)) from exc

    def screencap_raw(self):
        result = self._run('exec-out', 'screencap', timeout=60, binary=True)
        if result.returncode != 0 or len(result.stdout) < 12:
            raise AdbError('screencap failed')
        return result.stdout
```

Add to `cli.py`'s parser:

```python
    f.add_argument('--pitch', type=int, help='row pitch in bytes; defaults to width*4')
```

and in `_dispatch`:

```python
    if args.command == 'frame':
        from . import frames
        if args.panel:
            adb = _adb(args)
            pixels, w, h, fmt = frames.parse_screencap(adb.screencap_raw())
            out = frames.digest_rgba(pixels, w, h, want_ascii=args.ascii)
            out['source'] = 'panel'
        elif args.raw:
            data = Path(args.raw).read_bytes()
            out = frames.digest_rgba(data, args.width, args.height,
                                     pitch=args.pitch or args.width * 4, want_ascii=args.ascii)
            out['source'] = 'file'
        else:
            raise SystemExit(2)
        print(json.dumps(out, sort_keys=True))
        return 0
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_cli.py' -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/cli.py tools/diagkit/device.py tools/tests/test_diag_cli.py
git commit -m "diag frame: the panel and a file digest into the same record"
```

---

## Phase 3 - In-app

### Task 12: Agent `snapshot` and `mark`

**Files:**
- Create: `app/src/main/java/com/droiddeck/launcher/agent/AgentDiagnostics.kt`
- Modify: `app/src/main/java/com/droiddeck/launcher/agent/AgentBridgeProvider.kt`, `docs/agent-control.md`
- Test: `tools/tests/test_diag_agent.py` (source assertions, in the existing house style)

**Interfaces:**
- Produces (Kotlin): `object AgentDiagnostics { fun snapshot(context: Context, dir: File?): File; fun mark(context: Context, label: String): Boolean }`
- Provider methods: `"snapshot"`, `"mark"` (arg = label).

- [ ] **Step 1: Write the failing test**

```python
import unittest
from pathlib import Path

AGENT = Path(__file__).resolve().parents[2] / 'app/src/main/java/com/droiddeck/launcher/agent'


class AgentDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.src = (AGENT / 'AgentDiagnostics.kt').read_text(encoding='utf-8')
        self.provider = (AGENT / 'AgentBridgeProvider.kt').read_text(encoding='utf-8')

    def test_the_provider_exposes_snapshot_and_mark(self):
        self.assertIn('"snapshot"', self.provider)
        self.assertIn('"mark"', self.provider)

    def test_snapshot_gathers_the_layers_and_the_graphics_processes(self):
        for probe in ('dumpsys SurfaceFlinger', 'dumpsys display', 'dumpsys input', '/proc'):
            self.assertIn(probe, self.src)

    def test_snapshot_is_text_only(self):
        self.assertNotIn('screencap', self.src)

    def test_mark_writes_to_the_session_event_stream(self):
        self.assertIn('SessionEvents', self.src)
        self.assertIn('diag.mark', self.src)

    def test_the_provider_documents_the_new_methods(self):
        doc = (Path(__file__).resolve().parents[2] / 'docs/agent-control.md').read_text(encoding='utf-8')
        self.assertIn('--method snapshot', doc)
        self.assertIn('--method mark', doc)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_agent.py' -v`
Expected: FAIL - `AgentDiagnostics.kt` missing.

- [ ] **Step 3: Implement**

`AgentDiagnostics.kt`:

```kotlin
package com.droiddeck.launcher.agent

import android.content.Context
import android.util.Log
import com.droiddeck.launcher.session.SessionEvents
import com.droiddeck.launcher.session.SessionPaths
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * One call that gathers, as text, what a device looks like at an instant.
 *
 * The reason this exists: several sessions were diagnosed from a crash report plus whatever log
 * happened to survive, and the question "what did the display path actually look like at that
 * moment" could not be answered afterwards. Snapshot writes the answer where the session's other
 * artifacts go. It is text only - no image is taken, ever (see the project's no-screenshot rule).
 */
object AgentDiagnostics {
    private const val TAG = "AgentDiagnostics"

    private val PROBES = listOf(
        "dumpsys SurfaceFlinger" to "surfaceflinger.txt",
        "dumpsys display" to "display.txt",
        "dumpsys input" to "input.txt",
        "dumpsys window displays" to "windows.txt",
    )

    /** Writes a snapshot into [dir] (the session folder) and returns the directory it used. */
    @Synchronized
    fun snapshot(context: Context, dir: File?): File {
        val target = dir ?: SessionPaths.current()
            ?: File(context.filesDir, "diag-snapshots").apply { mkdirs() }
        target.mkdirs()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val out = File(target, "snapshot-$stamp")
        out.mkdirs()
        File(out, "procs.txt").writeText(processTable())
        PROBES.forEach { (command, name) ->
            runCatching { File(out, name).writeText(shell(command)) }
                .onFailure { Log.w(TAG, "probe failed: $command", it) }
        }
        SessionEvents.record("diag.snapshot", mapOf("dir" to out.absolutePath), target)
        return out
    }

    /** A marker in the session's event stream, so a host action has a place on the timeline. */
    fun mark(context: Context, label: String): Boolean {
        SessionEvents.record("diag.mark", mapOf("label" to label))
        return true
    }

    private fun shell(command: String): String {
        val process = ProcessBuilder("/system/bin/sh", "-c", command)
            .redirectErrorStream(true).start()
        val text = process.inputStream.bufferedReader().readText()
        process.waitFor()
        return text
    }

    /** Every process holding a graphics library, with its role and its command line. */
    private fun processTable(): String {
        val out = StringBuilder()
        for (dir in File("/proc").listFiles()?.filter { it.name.all(Char::isDigit) } ?: emptyList()) {
            val cmdline = runCatching { File(dir, "cmdline").readText() }.getOrNull() ?: continue
            if (!IGNORED.any { cmdline.contains(it) }) continue
            val comm = runCatching { File(dir, "comm").readText().trim() }.getOrNull() ?: "?"
            val role = when {
                cmdline.contains("--type=gpu-process") -> "gpu"
                cmdline.contains("--type=renderer") -> "renderer"
                cmdline.contains("--type=zygote") -> "zygote"
                cmdline.contains("--type=utility") -> "utility"
                else -> "browser"
            }
            val maps = runCatching { File(dir, "maps").readLines() }.getOrNull() ?: emptyList()
            val libs = maps.filter { line -> LIBS.any { line.contains(it) } }
                .map { it.substringAfterLast(' ') }
                .distinct()
            out.append("pid=${dir.name} comm=$comm role=$role\n")
            libs.forEach { out.append("  lib $it\n") }
        }
        return out.toString()
    }

    private val IGNORED = listOf("steam", "gamescope", "Xwayland", "steamwebhelper", "droiddeck")
    private val LIBS = listOf("libvulkan_powervr", "libhybris", "libgbm", "swiftshader")
}
```

In `AgentBridgeProvider.kt`, add to the `when (method)` block:

```kotlin
                "snapshot" -> AgentDiagnostics.snapshot(context, SessionPaths.current())
                    .let { JSONObject().put("ok", true).put("dir", it.absolutePath) }
                "mark" -> AgentDiagnostics.mark(context, arg ?: "unnamed")
                    .let { JSONObject().put("ok", it) }
```

and import `com.droiddeck.launcher.session.SessionPaths`.

Document both under `docs/agent-control.md`:

```markdown
### Diagnostics

```sh
adb shell content call --uri content://com.droiddeck.launcher.agent --method snapshot
adb shell content call --uri content://com.droiddeck.launcher.agent --method mark --arg "pre-restart"
```

`snapshot` writes `surfaceflinger.txt`, `display.txt`, `input.txt`, `windows.txt` and `procs.txt`
(every graphics process with the Vulkan/hybris/GBM libraries it has mapped) into a `snapshot-*`
directory in the current session folder. `mark` writes a `diag.mark` event into `events.jsonl`,
so a host action and the guest's own timeline share one correlation point. Neither takes an image.
```

- [ ] **Step 4: Build and run the tests**

Run: `JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew assembleDebug --console=plain`
Expected: BUILD SUCCESSFUL.

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_agent.py' -v`
Expected: PASS.

- [ ] **Step 5: Bump versionCode and commit**

Bump `versionCode` in `app/build.gradle` by one (the APK changed; see the CLAUDE.md rule).

```bash
git add app/src/main/java/com/droiddeck/launcher/agent/AgentDiagnostics.kt \
        app/src/main/java/com/droiddeck/launcher/agent/AgentBridgeProvider.kt \
        docs/agent-control.md app/build.gradle tools/tests/test_diag_agent.py
git commit -m "agent: snapshot and mark, so a device state can be captured mid-run as text"
```

---

### Task 13: The harness drives the app

**Files:**
- Modify: `tools/diagkit/runs.py`, `tools/diagkit/cli.py`
- Test: extend `tools/tests/test_diag_runs.py`

**Interfaces:**
- Produces: `Run.snapshot(adb) -> Path` (calls the provider, pulls the directory); `Run.mark(adb, label) -> None`; CLI `run --calibrate / --snapshot-on <phase>`.

- [ ] **Step 1: Write the failing test**

```python
class RunProviderTest(unittest.TestCase):
    def test_snapshot_calls_the_provider_and_records_it(self):
        import tempfile, shutil
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp, True)
        class A(FakeAdb):
            def provider(self, method, arg=None):
                return {'ok': True, 'dir': '/sdcard/snap1'}
            def pull(self, remote, local):
                Path(local).mkdir(parents=True, exist_ok=True)
                (Path(local) / 'procs.txt').write_text('pid=1\n')
                return Path(local)
        run = runs.Run(tmp, 'l', 'h', {'v': '1'}, A(), 'c', 41); run.begin()
        out = run.snapshot(adb=A()))
```

(This test is illustrative of the shape; write it against the real `Run` API you add - `run.snapshot(adb)` must record a manifest entry named `snapshot` and copy the pulled files under the run directory.)

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_runs.py' -v`
Expected: FAIL - no `snapshot`.

- [ ] **Step 3: Implement**

Add to `device.py`:

```python
    def provider(self, method, arg=None):
        """Call the debug agent ContentProvider through the shell's DUMP permission."""
        command = f'content call --uri content://com.droiddeck.launcher.agent --method {method}'
        if arg:
            command += f" --arg '{arg}'"
        out = self.shell(command)
        import json as _json
        marker = 'json='
        start = out.find(marker)
        if start < 0:
            raise AdbError(f'provider {method} returned no json')
        return _json.JSONDecoder().raw_decode(out[start + len(marker):].lstrip())[0]

    def pull(self, remote, local):
        result = self._run('pull', remote, str(local), timeout=120)
        if result.returncode != 0:
            raise AdbError(result.stderr.strip() or 'adb pull failed')
        return Path(local)
```

Add to `runs.py`:

```python
    def snapshot(self, adb):
        response = adb.provider('snapshot')
        if not response.get('ok'):
            manifest.mark_tainted(self.manifest, 'snapshot failed')
            return None
        dest = self.dir / 'snapshots' / Path(response['dir']).name
        adb.pull(response['dir'], dest)
        self.manifest.setdefault('snapshots', []).append(str(dest))
        manifest.write_manifest(self.dir, self.manifest)
        return dest

    def mark(self, adb, label):
        adb.provider('mark', label)
        self.manifest.setdefault('marks', []).append(label)
        manifest.write_manifest(self.dir, self.manifest)
```

`runs.py` needs `from pathlib import Path` (already imported).

Wire `--calibrate` in the CLI: after deployment, run each declared calibration, stamp it, and refuse to continue when one fails:

```python
def _calibrate(run, adb, instruments):
    from . import calibrate, frames
    from . import registry
    reg = registry.load()
    for name in instruments:
        kind = reg[name]['calibration']
        if kind == 'known-pattern':
            px = frames.pattern_rgba(320, 200)
            d = frames.digest_rgba(px, 320, 200)
            ok, detail = calibrate.check_digest(d, 320, 200)
        elif kind == 'known-marker':
            adb.provider('mark', f'calibrate-{name}')
            ok, detail = True, 'marker written'
        else:
            ok, detail = True, 'no host-side check; verified on device'
        manifest.stamp_instrument(run.manifest, name, 'runtime', ok, detail)
    manifest.write_manifest(run.dir, run.manifest)
    return all(manifest.instrument_trusted(run.manifest, n) for n in instruments)
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_runs.py' -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/runs.py tools/diagkit/device.py tools/diagkit/cli.py tools/tests/test_diag_runs.py
git commit -m "diag: the harness can snapshot and mark the device through the agent"
```

---

## Phase 4 - Analysis

### Task 14: `analyze` - findings that cite their evidence

**Files:**
- Create: `tools/diagkit/analyze.py`
- Test: `tools/tests/test_diag_analyze.py`

**Interfaces:**
- Produces: `finding(claim: str, artifact: str, line: int, trust: str) -> dict`; `merge_timeline(sources: dict[str, list[str]]) -> list[tuple[float, str, str, int]]`; `findings_from(run_dir: Path) -> list[dict]`; `render(findings: list[dict]) -> str`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, json, tempfile, shutil, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

analyze = load('analyze'); manifest = load('manifest')


class AnalyzeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_a_finding_carries_artifact_and_line(self):
        f = analyze.finding('the layer path composed a frame', 'wayland.log', 42, 'clean')
        self.assertEqual(('wayland.log', 42), (f['artifact'], f['line']))

    def test_a_finding_on_an_untrusted_instrument_is_marked(self):
        f = analyze.finding('zero rows present', 'frame.log', 7, 'untrusted')
        self.assertEqual('UNTRUSTED', f['stamp'])

    def test_a_finding_on_a_tainted_run_is_marked(self):
        f = analyze.finding('x', 'a.log', 1, 'tainted')
        self.assertEqual('TAINTED', f['stamp'])

    def test_the_timeline_merges_sources_in_time_order(self):
        merged = analyze.merge_timeline({'a.log': [(1.0, 'first')], 'b.log': [(0.5, 'zeroth')]})
        self.assertEqual(['zeroth', 'first'], [t[2] for t in merged])

    def test_rendering_shows_the_citation_for_every_row(self):
        text = analyze.render([analyze.finding('claim', 'a.log', 3, 'clean')])
        self.assertIn('a.log:3', text)

    def test_a_run_with_no_digest_reports_it_rather_than_guessing(self):
        run = self.tmp / 'run'; run.mkdir()
        (run / 'manifest.json').write_text(json.dumps(manifest.new_manifest(
            'l', 'h', {'v': '1'}, 'c', 41)))
        found = analyze.findings_from(run)
        self.assertTrue(any('no digest' in f['claim'] for f in found))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_analyze.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Implement**

```python
"""Turning a run's artifacts into claims, where every claim names the line it came from.

The rule enforced here is the one §73 says cost a section: before a hypothesis is written up,
check what is already on disk. A finding with no citation is not a finding; a finding resting on
an instrument that failed calibration, or on a run whose teardown failed, carries that on its face.
"""
from __future__ import annotations

import json
import re
from pathlib import Path


def finding(claim, artifact, line, trust):
    stamp = {'untrusted': 'UNTRUSTED', 'tainted': 'TAINTED'}.get(trust, '')
    return {'claim': claim, 'artifact': artifact, 'line': line, 'trust': trust, 'stamp': stamp}


def merge_timeline(sources):
    merged = []
    for name, entries in sources.items():
        for t, text in entries:
            merged.append((t, name, text, 0))
    merged.sort(key=lambda item: item[0])
    return [(t, name, text, i + 1) for i, (t, name, text, _) in enumerate(merged)]


TIME = re.compile(r'(\d{2}):(\d{2}):(\d{2})\.(\d{3})')


def _lines(path):
    try:
        return path.read_text(errors='replace').splitlines()
    except OSError:
        return []


def findings_from(run_dir):
    run_dir = Path(run_dir)
    m = json.loads((run_dir / 'manifest.json').read_text())
    tainted = 'tainted' if m['taint']['reasons'] else 'clean'
    out = []

    def untrusted(name):
        entry = m['instruments'].get(name)
        return bool(entry) and entry['calibration'] != 'passed'

    digest_lines = []
    for log in sorted(run_dir.rglob('*.log')):
        for i, line in enumerate(_lines(log), 1):
            if ' fd ' in line or line.startswith('fd '):
                digest_lines.append((log.relative_to(run_dir).as_posix(), i, line))

    if not digest_lines:
        out.append(finding('no digest was produced in this run: frame evidence is absent',
                           'manifest.json', 1, tainted))
    for artifact, line_no, line in digest_lines:
        if 'zero_rows=' in line:
            count = int(line.split('zero_rows=')[1].split()[0])
            severity = untrusted('frame-digest')
            out.append(finding(f'{count} wholly empty rows in the composed frame',
                               artifact, line_no, 'untrusted' if severity else tainted))
    return out


def render(findings):
    rows = ['claim | artifact:line | trust']
    for f in findings:
        stamp = f' [{f["stamp"]}]' if f['stamp'] else ''
        rows.append(f"{f['claim']} | {f['artifact']}:{f['line']} | {f['trust']}{stamp}")
    return '\n'.join(rows)
```

Add the `analyze` branch to `cli.py`:

```python
    if args.command == 'analyze':
        from . import analyze
        found = analyze.findings_from(Path(args.run))
        print(json.dumps({'ok': True, 'findings': found}, sort_keys=True))
        print(analyze.render(found), file=sys.stderr)
        return 0
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_analyze.py' -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/analyze.py tools/diagkit/cli.py tools/tests/test_diag_analyze.py
git commit -m "diag analyze: every finding names its artifact and line, or says why it cannot"
```

---

### Task 15: `prior` - search before hypothesising

**Files:**
- Create: `tools/diagkit/prior.py`
- Test: `tools/tests/test_diag_prior.py`

**Interfaces:**
- Produces: `index(roots: list[Path]) -> list[Path]`; `search(roots: list[Path], question: str, limit: int = 50) -> list[dict]` with `artifact`, `line`, `text`, `score`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, tempfile, shutil, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prior', TOOLS / 'diagkit/prior.py')
prior = importlib.util.module_from_spec(spec); spec.loader.exec_module(prior)


class PriorTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / 'a.log').write_text('one\ncompositor renders on PowerVR\nthree\n')
        (self.tmp / 'b.txt').write_text('nothing relevant\n')

    def test_a_hit_names_the_file_and_the_line(self):
        hits = prior.search([self.tmp], 'PowerVR')
        self.assertEqual(1, len(hits))
        self.assertEqual(('a.log', 2), (hits[0]['artifact'], hits[0]['line']))

    def test_search_is_case_insensitive(self):
        self.assertTrue(prior.search([self.tmp], 'powervr'))

    def test_no_hit_is_an_empty_list_not_an_error(self):
        self.assertEqual([], prior.search([self.tmp], 'zzz-not-here'))

    def test_multiple_words_rank_by_how_many_are_present(self):
        (self.tmp / 'c.log').write_text('PowerVR\n')
        hits = prior.search([self.tmp], 'PowerVR compositor')
        self.assertEqual('a.log', hits[0]['artifact'])
        self.assertGreater(hits[0]['score'], hits[1]['score'])

    def test_binary_files_are_skipped(self):
        (self.tmp / 'blob.bin').write_bytes(b'\x00\x01PowerVR\x00')
        self.assertEqual([], [h for h in prior.search([self.tmp], 'PowerVR') if h['artifact'] == 'blob.bin'])
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_prior.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Implement**

```python
"""Search the evidence already on disk before writing a hypothesis about it.

State doc §73: a hypothesis was written up and then refuted by a log line that had already been
pulled, in a file already in the tree. This is that check, mechanised, so it costs one command
instead of one section. Text extensions only - a binary blob is not evidence anyone can cite.
"""
from __future__ import annotations

from pathlib import Path

TEXT_SUFFIXES = {
    '.log', '.txt', '.json', '.jsonl', '.md', '.tsv', '.csv', '.xml', '.conf', '.ini',
    '.sh', '.py', '.c', '.h', '.kt', '.java', '.toml', '.yaml', '.yml',
}
SKIP_DIRS = {'.git', '__pycache__', 'build', '.gradle', '.kotlin', 'node_modules'}


def index(roots):
    found = []
    for root in roots:
        root = Path(root)
        if root.is_file():
            found.append(root)
            continue
        for path in root.rglob('*'):
            if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            found.append(path)
    return found


def search(roots, question, limit=50):
    terms = [t.lower() for t in question.split() if t]
    hits = []
    for path in index(roots):
        try:
            lines = path.read_text(errors='replace').splitlines()
        except OSError:
            continue
        for number, line in enumerate(lines, 1):
            low = line.lower()
            score = sum(1 for t in terms if t in low)
            if score:
                hits.append({'artifact': path.name, 'path': str(path),
                             'line': number, 'text': line.strip()[:400], 'score': score})
    hits.sort(key=lambda h: (-h['score'], h['artifact'], h['line']))
    return hits[:limit]
```

Add to `cli.py`:

```python
    if args.command == 'prior':
        from . import prior
        roots = [Path(r) for r in (args.roots or ['.', '../audit'])]
        hits = prior.search(roots, args.question)
        print(json.dumps({'ok': True, 'hits': hits}, sort_keys=True))
        print(f'{len(hits)} hit(s) - cite these before writing the hypothesis', file=sys.stderr)
        return 0
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_prior.py' -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/prior.py tools/diagkit/cli.py tools/tests/test_diag_prior.py
git commit -m "diag prior: search what is already on disk before forming a hypothesis"
```

---

### Task 16: `compare` - refusing an arm that varies two things

**Files:**
- Create: `tools/diagkit/compare.py`
- Test: `tools/tests/test_diag_compare.py`

**Interfaces:**
- Produces: `arms(run_dir: Path) -> dict[str, dict]`; `refusal(a: Path, b: Path) -> list[str]`; `diff_digests(a: dict, b: dict) -> list[str]`.

- [ ] **Step 1: Write the failing test**

```python
import importlib.util, json, tempfile, shutil, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

compare = load('compare'); manifest = load('manifest')


def run(tmp, name, arm, instruments=None):
    d = Path(tmp) / name; d.mkdir(parents=True)
    m = manifest.new_manifest(name, 'h', arm, 'c', 41)
    instruments = instruments or {}
    for k, v in instruments.items():
        manifest.stamp_instrument(m, k, 'id', v, 'd')
    (d / 'manifest.json').write_text(json.dumps(m))
    return d


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_two_runs_differing_in_one_declared_field_are_comparable(self):
        a = run(self.tmp, 'a', {'icd': 'A', 'shim': 'nulldev'}, {'frame-digest': True})
        b = run(self.tmp, 'b', {'icd': 'B', 'shim': 'nulldev'}, {'frame-digest': True})
        self.assertEqual([], compare.refusal(a, b))

    def test_two_differences_are_refused_with_both_named(self):
        a = run(self.tmp, 'a', {'icd': 'A', 'shim': 'nulldev'})
        b = run(self.tmp, 'b', {'icd': 'B', 'shim': 'ahb'})
        reasons = compare.refusal(a, b)
        self.assertEqual(1, len(reasons))
        self.assertIn('shim', reasons[0]); self.assertIn('icd', reasons[0])

    def test_a_differing_instrument_calibration_is_refused(self):
        """§63: an arm that also changed the logging looked like one variable and was not."""
        a = run(self.tmp, 'a', {'icd': 'A'}, {'frame-digest': True})
        b = run(self.tmp, 'b', {'icd': 'B'}, {'frame-digest': False})
        self.assertIn('frame-digest', compare.refusal(a, b)[0])

    def test_a_tainted_arm_is_refused(self):
        a = run(self.tmp, 'a', {'icd': 'A'}); b = run(self.tmp, 'b', {'icd': 'B'})
        m = json.loads((b / 'manifest.json').read_text()); manifest.mark_tainted(m, 'debris')
        (b / 'manifest.json').write_text(json.dumps(m))
        self.assertTrue(any('tainted' in r for r in compare.refusal(a, b)))

    def test_digest_diff_reports_which_fields_moved(self):
        a = {'zero_rows': 0, 'hash': 'sha256:a', 'grid': [[1] * 16] * 16}
        b = {'zero_rows': 12, 'hash': 'sha256:b', 'grid': [[1] * 16] * 16}
        moved = compare.diff_digests(a, b)
        self.assertIn('zero_rows', moved); self.assertIn('hash', moved)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_compare.py' -v`
Expected: FAIL - module missing.

- [ ] **Step 3: Implement**

```python
"""Two runs may be compared only when exactly one declared thing differs between them.

§63's matched A/B differed by one log line and a logging path that performed an extra round-trip
per call - a behavioural change in the "logging only" arm - and a survival difference was recorded
and later withdrawn. This refuses that shape of comparison instead of describing it.
"""
from __future__ import annotations

import json
from pathlib import Path


def _manifest(run_dir):
    return json.loads((Path(run_dir) / 'manifest.json').read_text())


def arms(run_dir):
    return _manifest(run_dir).get('arm') or {}


def _calibrations(m):
    return {k: v.get('calibration') for k, v in (m.get('instruments') or {}).items()}


def refusal(a, b):
    ma, mb = _manifest(a), _manifest(b)
    reasons = []
    arm_a, arm_b = ma.get('arm') or {}, mb.get('arm') or {}
    differing = sorted(k for k in set(arm_a) | set(arm_b) if arm_a.get(k) != arm_b.get(k))
    if len(differing) != 1:
        reasons.append('arms differ in %d fields (%s); a comparable pair differs in exactly one'
                       % (len(differing), ', '.join(differing) or 'none'))
    cal_a, cal_b = _calibrations(ma), _calibrations(mb)
    changed_instruments = sorted(k for k in set(cal_a) | set(cal_b) if cal_a.get(k) != cal_b.get(k))
    if changed_instruments:
        reasons.append('instrument calibration differs (%s), so the arms observe differently'
                       % ', '.join(changed_instruments))
    for name, m in (('a', ma), ('b', mb)):
        if m.get('taint', {}).get('reasons'):
            reasons.append(f'run {name} is tainted: ' + '; '.join(m['taint']['reasons']))
    return reasons


def diff_digests(a, b):
    keys = ['w', 'h', 'zero_rows', 'zero_runs', 'hash', 'grid', 'bands', 'rl']
    return [k for k in keys if a.get(k) != b.get(k)]
```

Add to `cli.py`:

```python
    if args.command == 'compare':
        from . import compare
        reasons = compare.refusal(Path(args.a), Path(args.b))
        print(json.dumps({'ok': not reasons, 'refusals': reasons}, sort_keys=True))
        return 0 if not reasons else 4
```

- [ ] **Step 4: Run the tests**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_compare.py' -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/compare.py tools/diagkit/cli.py tools/tests/test_diag_compare.py
git commit -m "diag compare: refuse an arm that varies more than the one thing it declares"
```

---

### Task 17: `export`, documentation, and the full suite

**Files:**
- Modify: `tools/diagkit/cli.py` (the `export` branch), `docs/development/tensor-g5-powervr-state.md`
- Test: extend `tools/tests/test_diag_cli.py`

**Interfaces:**
- Produces: CLI `diag export <run> [dest]` which writes a text-only bundle (manifest, logs, digests, findings) and refuses to include a binary.

- [ ] **Step 1: Write the failing test**

```python
class ExportTest(unittest.TestCase):
    def test_export_writes_text_only(self):
        import json, tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'; run.mkdir()
            (run / 'manifest.json').write_text(json.dumps({'schema': 1, 'arm': {}, 'instruments': {}, 'taint': {'reasons': []}}))
            (run / 'wayland.log').write_text('line\n')
            (run / 'frame.raw').write_bytes(b'\x00' * 64)
            dest = Path(tmp) / 'out'
            import contextlib, io
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(0, cli.main(['export', str(run), str(dest)]))
            names = {p.name for p in dest.rglob('*') if p.is_file()}
            self.assertIn('wayland.log', names)
            self.assertNotIn('frame.raw', names)
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 -m unittest discover -s tools/tests -p 'test_diag_cli.py' -v`
Expected: FAIL.

- [ ] **Step 3: Implement**

```python
    if args.command == 'export':
        from . import analyze
        run_dir = Path(args.run)
        dest = Path(args.dest) if args.dest else run_dir.with_name(run_dir.name + '-export')
        dest.mkdir(parents=True, exist_ok=True)
        copied = []
        for path in run_dir.rglob('*'):
            if not path.is_file():
                continue
            if path.suffix.lower() in {'.raw', '.png', '.jpg', '.bin', '.so'}:
                continue
            target = dest / path.relative_to(run_dir)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
            copied.append(str(path.relative_to(run_dir)))
        (dest / 'findings.txt').write_text(analyze.render(analyze.findings_from(run_dir)))
        print(json.dumps({'ok': True, 'dest': str(dest), 'files': sorted(copied)}, sort_keys=True))
        return 0
```

Append a section to `docs/development/tensor-g5-powervr-state.md` recording the harness: what a
run guarantees, why each of the four failure classes has a mechanism, and the commands. Use the
file's existing section numbering and style - one more numbered section, dated 2026-10-24.

- [ ] **Step 4: Run the whole suite**

Run: `python3 -m unittest discover -s tools/tests -p 'test_*.py' -v`
Expected: PASS, with the parity test either passing or skipping cleanly when no C compiler is present (it must NOT fail on a machine without one).

Run: `JAVA_HOME=~/.jdks/jdk-17.0.20+8 ./gradlew test --console=plain`
Expected: BUILD SUCCESSFUL.

- [ ] **Step 5: Commit**

```bash
git add tools/diagkit/cli.py docs/development/tensor-g5-powervr-state.md tools/tests/test_diag_cli.py
git commit -m "diag: export, and the state doc records what a run now guarantees"
```

---

## Self-review

**Spec coverage**

| Spec section | Task |
|---|---|
| Instrument registry | 4 |
| Calibration gate, all five checks | 3, 8, 10 (pattern/digest), 12 (markers) |
| Manifest, before and after the run | 1, 6 |
| Proof of what ran (maps inode) | 6 (`deployed_maps`), 5 (inventory) |
| Frame digest schema, true resolution, pitch | 2, 9, 10 |
| Adaptive cadence | 10 (`fd_should_sample`) |
| Panel producer, never stored | 11 |
| `doctor`, `run`, `prior`, `analyze`, `compare`, `export` | 5, 6, 15, 14, 16, 17 |
| Agent `snapshot` / `mark` | 12 |
| Digest control from the agent | 13 (calibration/marks), 10 (env gate) |
| Retention (cap raw, keep digest) | 17 (`export` excludes binaries); raw pixels are never written by design |
| Toolchain constraints | Task 8 (WSL/zig), 10 (Gradle/JDK 17), Global Constraints |
| Testing strategy | every task; parity in 9; skip-clean in 17 |

**Known gap, stated rather than hidden:** the spec's "agent can turn the digest on/off per run" is
implemented as an environment gate (`BL_FRAME_DIGEST`), set through the existing
`Download/droiddeck-env` file. A live toggle over the provider would need a JNI call into the
compositor thread; it is not in this plan because the env gate already gives the unattended harness
what it needs (choose the level before the run). If a mid-run change proves necessary, that is a
new task, not a silent omission.

**Type consistency:** `digest_rgba` returns `bands` as `[mean, spread, zero_rows]` in Tasks 2, 3, 9
and 10 (C: `bands[i][0..2]`); `finding()` uses `artifact`/`line`/`trust`/`stamp` in Tasks 14 and 17;
`refusal()` and `arms()` agree with the CLI in Task 16; `Run`'s methods (`begin`, `deploy`,
`deployed_maps`, `finish`, `snapshot`, `mark`) are consistent between Tasks 6 and 13.

---

## Execution handoff

Two options:

1. **Subagent-Driven (recommended)** - a fresh subagent per task, review between tasks.
2. **Inline Execution** - run the tasks in this session, batching with checkpoints.
