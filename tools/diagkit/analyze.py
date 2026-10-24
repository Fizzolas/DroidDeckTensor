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
            merged.append((t, name, text))
    merged.sort(key=lambda item: item[0])
    return [(t, name, text, i + 1) for i, (t, name, text) in enumerate(merged)]


TIME = re.compile(r'(\d{2}):(\d{2}):(\d{2})\.(\d{3})')


def _lines(path):
    try:
        return path.read_text(errors='replace').splitlines()
    except OSError:
        return []


def _digest_records(run_dir):
    """Every `fd` digest line the run produced, with where it came from."""
    records = []
    for log in sorted(Path(run_dir).rglob('*')):
        if not log.is_file() or log.suffix.lower() not in {'.log', '.txt', '.jsonl'}:
            continue
        for number, line in enumerate(_lines(log), 1):
            if ' fd ' in line or re.match(r'^\S+ +fd +', line) or line.startswith('fd '):
                records.append((log.relative_to(run_dir).as_posix(), number, line))
    return records


def findings_from(run_dir):
    run_dir = Path(run_dir)
    manifest_path = run_dir / 'manifest.json'
    if manifest_path.is_file():
        m = json.loads(manifest_path.read_text())
    else:
        m = {'taint': {'reasons': ['no manifest']}, 'instruments': {}}
    tainted = 'tainted' if m.get('taint', {}).get('reasons') else 'clean'

    def untrusted(name):
        entry = (m.get('instruments') or {}).get(name)
        return bool(entry) and entry['calibration'] != 'passed'

    out = []
    records = _digest_records(run_dir)
    if not records:
        out.append(finding('no digest was produced in this run: frame evidence is absent',
                           'manifest.json', 1, tainted))
        return out

    digest_trust = 'untrusted' if untrusted('frame-digest') else tainted
    worst_zero = None
    for artifact, number, line in records:
        match = re.search(r'zero_rows=(\d+)', line)
        if not match:
            continue
        count = int(match.group(1))
        if count == 0:
            continue
        if worst_zero is None or count > worst_zero[0]:
            worst_zero = (count, artifact, number)
    if worst_zero:
        count, artifact, number = worst_zero
        out.append(finding(f'{count} wholly empty rows in the composed frame',
                           artifact, number, digest_trust))
    else:
        first = records[0]
        out.append(finding('every composed frame digest reports no empty rows',
                           first[0], first[1], digest_trust))

    marks = m.get('marks') or []
    if marks:
        out.append(finding(f'{len(marks)} correlation mark(s) were written by the host',
                           'manifest.json', 1, tainted))
    return out


def render(findings):
    rows = ['claim | artifact:line | trust']
    for f in findings:
        stamp = f' [{f["stamp"]}]' if f['stamp'] else ''
        rows.append(f"{f['claim']} | {f['artifact']}:{f['line']} | {f['trust']}{stamp}")
    return '\n'.join(rows)
