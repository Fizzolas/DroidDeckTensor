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
