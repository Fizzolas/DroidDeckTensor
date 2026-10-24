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
    {'name': 'calibrate', 'app_path': f'{GUEST}/tmp/calibrate', 'stock': None},
    {'name': 'cdp_probe.py', 'app_path': f'{GUEST}/tmp/cdp_probe.py', 'stock': None},
    {'name': 'x-inspect', 'app_path': f'{GUEST}/tmp/xinspect', 'stock': None},
    {'name': 'proc_watch', 'app_path': f'{GUEST}/tmp/proc_watch.sh', 'stock': None},
    {'name': 'gbm-shim', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/libgbm.so.1', 'stock': None},
    {'name': 'hybris-copy', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/libhybris.so', 'stock': None},
    {'name': 'q-copy', 'app_path': f'{GUEST}/root/.local/share/Steam/steamrtarm64/q.so', 'stock': None},
    {'name': 'droiddeck-env', 'app_path': '/sdcard/Download/droiddeck-env', 'stock': None},
]


ENV_PATH = '/sdcard/Download/droiddeck-env'


def inventory(adb):
    entries = []
    for artifact in ARTIFACTS:
        digest = adb.sha256(artifact['app_path'])
        entry = {
            'name': artifact['name'],
            'path': artifact['app_path'],
            'present': digest is not None,
            'sha256': digest,
            'stock': artifact['stock'],
        }
        if artifact['name'] == 'droiddeck-env' and digest is not None:
            entry['lines'] = read_env(adb)
        entries.append(entry)
    return entries


def read_env(adb):
    """The lines of the env switch file, or None when there is no file."""
    try:
        out = adb.shell(f'cat {ENV_PATH} 2>/dev/null')
    except Exception:
        return None
    lines = [line.strip() for line in out.splitlines()]
    return [line for line in lines if line and not line.startswith('#')]


def verdict(entries, expected_env=None):
    """Clean unless something is present that the caller did not declare.

    `droiddeck-env` is the one artifact that is not debris by definition: it is how an experiment
    is armed. `expected_env` names the lines a run is about to use, and matching them exactly makes
    the file declared rather than leftover - otherwise every armed run would refuse the next one.
    """
    reasons = []
    for entry in entries:
        if not entry['present']:
            continue
        if entry['stock'] and entry['sha256'] == entry['stock']:
            continue
        if entry['name'] == 'droiddeck-env' and expected_env is not None:
            entry['expected'] = list(expected_env)
            entry['matchesExpected'] = entry.get('lines') == list(expected_env)
            if entry['matchesExpected']:
                continue
            reasons.append(f"droiddeck-env holds {entry.get('lines')} but the run declared {list(expected_env)}")
            continue
        reasons.append(f"{entry['name']} is present at {entry['path']} ({entry['sha256']})")
    return ('dirty' if reasons else 'clean', reasons)
