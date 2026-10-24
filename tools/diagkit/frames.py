"""A frame as text: the same record from the compositor and from the panel.

Why this exists: three findings in the state doc were built on a readback helper sized for a
256x256 cursor, and all three were retracted (§65-68). The rule that prevents a repeat is that a
digest is *checked against a known pattern* (see `calibrate`), and that it takes the producer's
real pitch instead of assuming a tightly packed row. The second producer is `screencap`, decoded
here and never stored: the panel is the only independent view and §70 is what settled a week of
argument with it.

The C implementation in app/src/main/cpp/waylandcomp/src/frame_digest.c must produce exactly the
same integers; test_diag_frames_parity.py holds the two to it.
"""
from __future__ import annotations

import hashlib
import re
import struct

RAMP = ' .:-=+*#%@'

# Run lengths are bucketed so the profile is a fixed-size, integer record.
RL_BUCKETS = (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)


def _row_means(pixels, w, h, pitch):
    """Per-row mean luminance and whether the row is wholly zero."""
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


def _bucket(n):
    for i, cap in enumerate(RL_BUCKETS):
        if n <= cap:
            return i
    return len(RL_BUCKETS) - 1


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
        if y1 <= y0:
            y1 = y0 + 1
        chunk = means[y0:min(y1, h)] or [0]
        mean = sum(chunk) // len(chunk)
        spread = max(chunk) - min(chunk)
        band_rows.append([mean, spread, sum(1 for z in zeros[y0:min(y1, h)] if z)])

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


# The grid is 256 cells x 2 hex digits = 1024 characters, but droiddeck_log's message buffer is
# 512 bytes, so the compositor emits it as eight `gridN=<64 hex>` halves. A single-line reader saw
# a 506-character line and could not decode it (measured on the device).
GRID_CHUNK = re.compile(r'grid(\d)=([0-9a-f]+)')


def parse_grid(line):
    """Decode one chunk of the compositor's luminance-grid record, or None.

    Returns (half_index, rows) where `rows` is that chunk's two rows. Use `parse_grid_lines` to
    assemble the whole 16x16 grid from a batch of lines.
    """
    match = GRID_CHUNK.search(line)
    if not match:
        return None
    half = int(match.group(1))
    hexes = match.group(2)
    if half > 7 or len(hexes) != 2 * 16 * 2:
        return None
    values = [int(hexes[i:i + 2], 16) for i in range(0, len(hexes), 2)]
    return half, [values[0:16], values[16:32]]


def parse_grid_lines(lines):
    """Assemble the most recent complete 16x16 grid from a run of log lines, or None."""
    parts = {}
    complete = None
    for line in lines:
        parsed = parse_grid(line)
        if not parsed:
            continue
        half, rows = parsed
        if half == 0:
            parts = {}
        parts[half] = rows
        if len(parts) == 8:
            complete = [row for i in range(8) for row in parts[i]]
    return complete


def parse_grid_single(line):
    """Decode a one-line `grid=<1024 hex>` record (the shape before the 512-byte split)."""
    match = re.search(r'grid=([0-9a-f]{1024})', line)
    if not match:
        return None
    hexes = match.group(1)
    values = [int(hexes[i:i + 2], 16) for i in range(0, len(hexes), 2)]
    return [values[r * 16:(r + 1) * 16] for r in range(16)]


def _flat(grid):
    return [v for row in grid for v in row]


def _corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    da = [v - ma for v in a]
    db = [v - mb for v in b]
    va = sum(v * v for v in da)
    vb = sum(v * v for v in db)
    if va <= 0 or vb <= 0:
        return 0.0
    return sum(x * y for x, y in zip(da, db)) / (va ** 0.5 * vb ** 0.5)


def _rotate(grid, quarter_turns):
    out = [row[:] for row in grid]
    for _ in range(quarter_turns % 4):
        out = [list(row) for row in zip(*out[::-1])]
    return out


def correlate(a, b):
    """How well two digests describe the same picture, trying the four quarter turns.

    The two producers do not share a frame of reference: the scene is 1650x732 and the panel
    1080x2404, one the other rotated. Comparing raw extents is therefore meaningless - what is
    comparable is the 16x16 luminance grid, at whichever quarter turn lines them up.

    A correlation is only worth reading when BOTH pictures have structure. Two uniform black
    frames correlate at 1.0 and prove nothing, so `discriminating` gates the verdict on the
    variance of both; this is the same trap that made a blank screencap look like a match before.
    """
    ga = (a or {}).get('grid') or []
    gb = (b or {}).get('grid') or []
    if not ga or not gb or len(ga) != len(gb) or any(len(r) != len(c) for r, c in zip(ga, gb)):
        raise ValueError('both digests need a grid of the same shape')

    flat_a = _flat(ga)
    n = len(flat_a)
    var_a = sum((v - sum(flat_a) / n) ** 2 for v in flat_a) / n
    var_b_identity = sum((v - sum(_flat(gb)) / n) ** 2 for v in _flat(gb)) / n
    discriminating = var_a >= 50 and var_b_identity >= 50

    best = None
    for turns in range(4):
        r = _corr(flat_a, _flat(_rotate(gb, turns)))
        if best is None or r > best[0]:
            best = (r, turns)
    r, turns = best

    if not discriminating:
        verdict = 'not discriminating: one or both pictures are flat, so agreement means nothing'
    elif r >= 0.8:
        verdict = 'same picture'
    elif r <= 0.2:
        verdict = 'differs'
    else:
        verdict = 'partly related'
    return {
        'r': round(r, 4),
        'quarter_turns': turns,
        'discriminating': discriminating,
        'variance': [round(var_a, 1), round(var_b_identity, 1)],
        'extents': [[(a or {}).get('w'), (a or {}).get('h')], [(b or {}).get('w'), (b or {}).get('h')]],
        'verdict': verdict,
    }


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
