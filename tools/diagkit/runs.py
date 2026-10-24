"""A run is a transaction: begin, deploy, verify, finish, and say whether the device is restored.

The failure this prevents is the recorded one (§63): a rollback ran while the device flaked off
USB, failed, and two sessions executed with the previous configuration still installed while the
notes said otherwise. Here a teardown that did not verify is not an error to mention - it is a
taint that makes the run's evidence ineligible as sole proof.
"""
from __future__ import annotations

import importlib.util
from datetime import datetime, timezone
from pathlib import Path

try:  # imported as part of the package
    from . import manifest
except ImportError:  # loaded by file path in tests
    _spec = importlib.util.spec_from_file_location(
        'manifest', Path(__file__).with_name('manifest.py'))
    manifest = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(manifest)


def arm_differs(a, b):
    keys = sorted(set(a) | set(b))
    return [k for k in keys if a.get(k) != b.get(k)]


def slugify(label, limit=48):
    slug = ''.join(c if c.isalnum() or c in '-_' else '-' for c in label)
    return slug[:limit].strip('-')


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
        self.dir = self.root / f'{stamp}-{slugify(self.label)}'
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

    def snapshot(self, adb=None):
        """Ask the app for a text snapshot of the device and file it with the run."""
        adb = adb or self.adb
        response = adb.provider('snapshot')
        if not response.get('ok'):
            manifest.mark_tainted(self.manifest, 'snapshot failed')
            manifest.write_manifest(self.dir, self.manifest)
            return None
        dest = self.dir / 'snapshots' / Path(response['dir']).name
        dest.parent.mkdir(parents=True, exist_ok=True)
        adb.pull(response['dir'], dest)
        self.manifest.setdefault('snapshots', []).append(str(dest))
        manifest.write_manifest(self.dir, self.manifest)
        return dest

    def mark(self, adb, label):
        """Put a labelled point on the session's own timeline, so host and guest share a clock."""
        adb = adb or self.adb
        adb.provider('mark', label)
        self.manifest.setdefault('marks', []).append(label)
        manifest.write_manifest(self.dir, self.manifest)
        return label

    def finish(self, teardown_ok, reasons):
        self.manifest['teardown'] = 'ok' if teardown_ok else 'failed'
        for reason in reasons:
            manifest.mark_tainted(self.manifest, f'teardown: {reason}')
        self.manifest['finishedAt'] = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ')
        manifest.write_manifest(self.dir, self.manifest)
        return self.manifest
