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
    changed = sorted(k for k in set(cal_a) | set(cal_b) if cal_a.get(k) != cal_b.get(k))
    if changed:
        reasons.append('instrument calibration differs (%s), so the arms observe differently'
                       % ', '.join(changed))
    for name, m in (('a', ma), ('b', mb)):
        if m.get('taint', {}).get('reasons'):
            reasons.append(f'run {name} is tainted: ' + '; '.join(m['taint']['reasons']))
    return reasons


def diff_digests(a, b):
    keys = ['w', 'h', 'zero_rows', 'zero_runs', 'hash', 'grid', 'bands', 'rl']
    return [k for k in keys if a.get(k) != b.get(k)]
