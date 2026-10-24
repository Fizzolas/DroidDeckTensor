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
