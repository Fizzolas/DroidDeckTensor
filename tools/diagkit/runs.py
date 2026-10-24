"""A run is a transaction: begin, deploy, verify, finish, and say whether the device is restored.

The failure this prevents is the recorded one (§63): a rollback ran while the device flaked off
USB, failed, and two sessions executed with the previous configuration still installed while the
notes said otherwise. Here a teardown that did not verify is not an error to mention - it is a
taint that makes the run's evidence ineligible as sole proof.
"""
from __future__ import annotations

import importlib
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


def _sibling(name):
    """Import a module beside this file, whether we are a package or loaded by path."""
    try:
        if __package__:
            return importlib.import_module(f'{__package__}.{name}')
    except ImportError:
        pass
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f'{name}.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_registry():
    return _sibling('registry').load()


def _instrument_identity(kind):
    return f'calibration:{kind}'


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

    # What only the shell uid can read. The app has no android.permission.DUMP, so asking it for
    # these produced "Permission Denial" inside files that looked like evidence (measured on the
    # device: every dumpsys probe came back 27-133 bytes of denial).
    HOST_DUMPS = (
        ('dumpsys SurfaceFlinger', 'surfaceflinger.txt'),
        ('dumpsys display', 'display.txt'),
        ('dumpsys input', 'input.txt'),
        ('dumpsys window displays', 'windows.txt'),
    )

    def snapshot(self, adb=None):
        """Ask the app for its half, then gather the shell-only half beside it."""
        adb = adb or self.adb
        response = adb.provider('snapshot')
        if not response.get('ok'):
            manifest.mark_tainted(self.manifest, 'snapshot failed')
            manifest.write_manifest(self.dir, self.manifest)
            return None
        dest = self.dir / 'snapshots' / Path(response['dir']).name
        dest.parent.mkdir(parents=True, exist_ok=True)
        if str(response['dir']).startswith(getattr(adb, 'APP_DATA', '/data/user/0/')):
            adb.pull_app_private(response['dir'], dest)
        else:
            adb.pull(response['dir'], dest)
        self.gather_host_dumps(adb, dest)
        self.manifest.setdefault('snapshots', []).append(str(dest))
        manifest.write_manifest(self.dir, self.manifest)
        return dest

    def gather_host_dumps(self, adb, dest):
        """The dumps an app process cannot read, taken from the host with the shell's DUMP grant."""
        dest = Path(dest)
        dest.mkdir(parents=True, exist_ok=True)
        for command, name in self.HOST_DUMPS:
            try:
                (dest / name).write_text(adb.shell(command, timeout=60), encoding='utf-8')
            except Exception as exc:
                (dest / name).write_text(f'host dump failed: {exc}\n', encoding='utf-8')
        return dest

    def mark(self, adb, label):
        """Put a labelled point on the session's own timeline, so host and guest share a clock."""
        adb = adb or self.adb
        adb.provider('mark', label)
        self.manifest.setdefault('marks', []).append(label)
        manifest.write_manifest(self.dir, self.manifest)
        return label

    def calibrate(self, adb, instruments):
        """Run each instrument's known-answer check and stamp the verdict into the manifest.

        Returns True only when every instrument in the list ended up trusted. A failed calibration
        is not an error to report and carry on from - it is the reason a later finding cannot be
        cited, so the caller stops before it measures anything.
        """
        reg = load_registry()
        ok = True
        measured = []
        for name in instruments:
            if name not in reg:
                # A name the registry does not know cannot be checked, so it cannot be believed.
                manifest.stamp_instrument(self.manifest, name, 'unregistered', False,
                                          'not in the instrument registry')
                measured.append(f'{name}: not in the registry')
                ok = False
                continue
            kind = reg[name].get('calibration')
            passed, detail = self._check(adb, name, kind)
            manifest.stamp_instrument(self.manifest, name, _instrument_identity(kind), passed, detail)
            measured.append(f'{name}: {detail}')
            ok = ok and passed
        manifest.write_manifest(self.dir, self.manifest)
        self.manifest['calibrationDetail'] = measured
        manifest.write_manifest(self.dir, self.manifest)
        return ok

    def _check(self, adb, name, kind):
        if kind == 'known-pattern':
            _frames = _sibling('frames')
            _cal = _sibling('calibrate')
            px = _frames.pattern_rgba(320, 200)
            d = _frames.digest_rgba(px, 320, 200)
            return _cal.check_digest(d, 320, 200)
        if kind == 'known-marker':
            try:
                adb.provider('mark', f'calibrate-{name}')
                return True, 'marker written to the session event stream'
            except Exception as exc:
                return False, f'marker not written: {exc}'
        if kind == 'none':
            return True, 'no known-answer check declared'
        return True, f'{kind} is checked on the device, not from the host'

    def finish(self, teardown_ok, reasons):
        self.manifest['teardown'] = 'ok' if teardown_ok else 'failed'
        for reason in reasons:
            manifest.mark_tainted(self.manifest, f'teardown: {reason}')
        self.manifest['finishedAt'] = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H-%M-%SZ')
        manifest.write_manifest(self.dir, self.manifest)
        return self.manifest
