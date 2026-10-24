"""adb, with the project's own device resolver and nothing that cannot be faked in a test."""
from __future__ import annotations

import json
import os
import shutil
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

    def _run(self, *args, timeout=30, binary=False):
        try:
            return subprocess.run([self.adb, '-s', self.serial, *args],
                                  capture_output=True, text=not binary,
                                  check=False, timeout=timeout)
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

    def screencap_raw(self):
        """Raw RGBA from SurfaceFlinger. The caller decodes it and must not persist it."""
        result = self._run('exec-out', 'screencap', timeout=60, binary=True)
        if result.returncode != 0 or len(result.stdout) < 12:
            raise AdbError('screencap failed')
        return result.stdout

    def provider(self, method, arg=None):
        """Call the debug agent ContentProvider through the shell's DUMP permission."""
        command = f'content call --uri content://com.droiddeck.launcher.agent --method {method}'
        if arg:
            command += f" --arg '{arg}'"
        out = self.shell(command)
        marker = 'json='
        start = out.find(marker)
        if start < 0:
            raise AdbError(f'provider {method} returned no json')
        return json.JSONDecoder().raw_decode(out[start + len(marker):].lstrip())[0]

    def pull(self, remote, local):
        result = self._run('pull', remote, str(local), timeout=120)
        if result.returncode != 0:
            raise AdbError(result.stderr.strip() or 'adb pull failed')
        return Path(local)

    APP_DATA = '/data/user/0/com.droiddeck.launcher/'

    def pull_app_private(self, remote, local):
        """Pull a path inside the app's own data dir, which `adb pull` cannot reach.

        The app's files are mode 600 under its uid, so the shell user gets "Permission denied"
        (measured on the device). `run-as` runs as that uid, so the directory is tarred in the
        guest and extracted here. tarfile is stdlib, so nothing new is needed to read it.
        """
        import io
        import tarfile

        remote = str(remote)
        relative = remote[len(self.APP_DATA):] if remote.startswith(self.APP_DATA) else remote
        result = self._run('exec-out', f'run-as com.droiddeck.launcher tar -cf - {relative}',
                           timeout=120, binary=True)
        if result.returncode != 0:
            detail = result.stderr.decode(errors='replace').strip() if result.stderr else ''
            raise AdbError(detail or 'run-as tar failed')
        local = Path(local)
        local.mkdir(parents=True, exist_ok=True)
        with tarfile.open(fileobj=io.BytesIO(result.stdout), mode='r:') as archive:
            archive.extractall(local, filter='data')
        # tar keeps the guest's whole path (files/diag-snapshots/<name>/...), so the payload has
        # to be moved up to the directory the caller asked for; otherwise the snapshot arrives
        # nested and every later path in it is wrong.
        wanted = Path(relative).name
        nested = next((p for p in local.rglob(wanted) if p.is_dir()), None)
        if nested and nested != local:
            for child in list(nested.iterdir()):
                target = local / child.name
                if target.exists():
                    continue
                shutil.move(str(child), str(target))
            for stray in [nested, *nested.parents]:
                if stray == local:
                    break
                shutil.rmtree(stray, ignore_errors=True)
        return local
