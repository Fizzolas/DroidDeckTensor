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
