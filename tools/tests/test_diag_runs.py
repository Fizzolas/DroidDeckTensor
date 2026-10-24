import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


manifest = load('manifest')
runs = load('runs')


class FakeAdb:
    def __init__(self):
        self.files = {}
        self.written = []
        self.provider_calls = []
        self.calls = []

    def shell(self, cmd, timeout=30):
        self.calls.append(cmd)
        return ''

    def push_bytes(self, path, data):
        self.written.append((path, data))
        self.files[path] = manifest.sha256_bytes(data)

    def sha256(self, path):
        return self.files.get(path)

    def rm(self, path):
        self.files.pop(path, None)

    def provider(self, method, arg=None):
        self.provider_calls.append((method, arg))
        if method == 'snapshot':
            return {'ok': True, 'dir': '/sdcard/snap1'}
        return {'ok': True}

    def pull(self, remote, local):
        Path(local).mkdir(parents=True, exist_ok=True)
        (Path(local) / 'procs.txt').write_text('pid=1\n')
        return Path(local)


class RunTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.adb = FakeAdb()

    def make(self, arm=None):
        return runs.Run(self.tmp, 'display path', 'the layer path presents',
                        arm or {'name': 'icd', 'value': 'display-layer'},
                        self.adb, 'abc123', 40)

    def test_begin_creates_a_manifest_and_a_directory(self):
        run = self.make()
        d = run.begin()
        self.assertTrue((d / 'manifest.json').is_file())
        self.assertEqual('clean', manifest.trust(manifest.read_manifest(d)))

    def test_a_push_that_lands_with_the_expected_hash_is_clean(self):
        run = self.make()
        run.begin()
        data = b'\x7fELF'
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', data, manifest.sha256_bytes(data))
        self.assertEqual('clean', manifest.trust(run.manifest))

    def test_a_push_that_lands_with_a_different_hash_taints(self):
        run = self.make()
        run.begin()
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'\x7fELF', 'sha256:expectsomethingelse')
        self.assertEqual('tainted', manifest.trust(run.manifest))

    def test_a_process_loading_an_unexpected_inode_taints(self):
        run = self.make()
        run.begin()
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'x', manifest.sha256_bytes(b'x'))
        run.deployed_maps({'steam': ['sha256:' + 'a' * 64]})
        self.assertEqual('tainted', manifest.trust(run.manifest))

    def test_a_process_loading_the_deployed_inode_does_not_taint(self):
        run = self.make()
        run.begin()
        sha = manifest.sha256_bytes(b'x')
        run.deploy('crashcatch', 'files/tmp/crashcatch.so', b'x', sha)
        run.deployed_maps({'steam': [sha]})
        self.assertEqual('clean', manifest.trust(run.manifest))

    def test_a_failed_teardown_taints_and_is_recorded(self):
        run = self.make()
        run.begin()
        m = run.finish(teardown_ok=False, reasons=['shim still present'])
        self.assertEqual('tainted', manifest.trust(m))
        self.assertEqual('failed', m['teardown'])

    def test_a_snapshot_is_called_filed_and_recorded(self):
        run = self.make()
        run.begin()
        out = run.snapshot()
        self.assertIsNotNone(out)
        self.assertTrue((Path(out) / 'procs.txt').is_file())
        self.assertIn(('snapshot', None), self.adb.provider_calls)
        self.assertEqual('clean', manifest.trust(manifest.read_manifest(run.dir)))

    def test_the_host_gathers_the_dumps_an_app_cannot_read(self):
        """An app holds no DUMP permission; measured on device, every dumpsys probe was a denial."""
        run = self.make()
        run.begin()
        out = run.snapshot()
        for _command, name in run.HOST_DUMPS:
            self.assertTrue((Path(out) / name).is_file(), name)
        self.assertIn('dumpsys SurfaceFlinger', ' '.join(self.adb.calls))

    def test_a_failed_host_dump_is_recorded_not_silently_empty(self):
        class NoDump(FakeAdb):
            def shell(self, cmd, timeout=30):
                self.calls.append(cmd)
                if cmd.startswith('dumpsys'):
                    raise RuntimeError('denied')
                return ''
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, NoDump(), 'c', 41)
        run.begin()
        out = run.snapshot()
        text = (Path(out) / 'surfaceflinger.txt').read_text()
        self.assertIn('host dump failed', text)

    def test_a_mark_is_recorded(self):
        run = self.make()
        run.begin()
        run.mark(None, 'pre-restart')
        self.assertIn(('mark', 'pre-restart'), self.adb.provider_calls)
        self.assertEqual(['pre-restart'], run.manifest['marks'])

    def test_arm_differs_reports_each_differing_field(self):
        self.assertEqual(['value'], runs.arm_differs(
            {'name': 'icd', 'value': 'A', 'shim': 'nulldev'},
            {'name': 'icd', 'value': 'B', 'shim': 'nulldev'}))

    def test_arm_differs_is_empty_for_one_variable(self):
        self.assertEqual([], runs.arm_differs({'value': 'A'}, {'value': 'A'}))

    def test_calibration_passes_for_a_known_pattern_instrument(self):
        run = self.make()
        run.begin()
        self.assertTrue(run.calibrate(self.adb, ['frame-digest']))
        self.assertTrue(manifest.instrument_trusted(run.manifest, 'frame-digest'))

    def test_calibration_marks_a_marker_instrument_through_the_agent(self):
        run = self.make()
        run.begin()
        self.assertTrue(run.calibrate(self.adb, ['snapshot']))
        self.assertIn(('mark', 'calibrate-snapshot'), self.adb.provider_calls)

    def test_a_marker_instrument_whose_agent_call_fails_is_untrusted(self):
        class Broken(FakeAdb):
            def provider(self, method, arg=None):
                raise RuntimeError('no provider')
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, Broken(), 'c', 41)
        run.begin()
        self.assertFalse(run.calibrate(run.adb, ['snapshot']))
        self.assertFalse(manifest.instrument_trusted(run.manifest, 'snapshot'))

    def test_an_unknown_instrument_is_not_trusted(self):
        run = self.make()
        run.begin()
        self.assertFalse(run.calibrate(self.adb, ['not-an-instrument']))

    def test_calibration_is_recorded_in_the_manifest_on_disk(self):
        run = self.make()
        run.begin()
        run.calibrate(self.adb, ['frame-digest'])
        stored = manifest.read_manifest(run.dir)
        self.assertEqual('passed', stored['instruments']['frame-digest']['calibration'])

    def test_a_snapshot_in_app_private_data_is_pulled_through_run_as(self):
        """adb pull gets Permission denied on the app's own files (measured on the device)."""
        class Private(FakeAdb):
            APP_DATA = '/data/user/0/com.droiddeck.launcher/'
            def __init__(self):
                super().__init__()
                self.app_pulls = []
            def provider(self, method, arg=None):
                self.provider_calls.append((method, arg))
                return {'ok': True, 'dir': self.APP_DATA + 'files/diag-snapshots/snapshot-1'}
            def pull_app_private(self, remote, local):
                self.app_pulls.append(remote)
                Path(local).mkdir(parents=True, exist_ok=True)
                (Path(local) / 'procs.txt').write_text('pid=1\n')
                return Path(local)
            def pull(self, remote, local):
                raise AssertionError('must not use a plain pull on app-private data')
        adb = Private()
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, adb, 'c', 41)
        run.begin()
        out = run.snapshot()
        self.assertTrue((Path(out) / 'procs.txt').is_file())
        self.assertTrue(adb.app_pulls)

    def test_teardown_restores_the_previous_env_and_verifies_the_device(self):
        """§63's failure: a rollback that did not verify left the device armed."""
        class EnvAdb(FakeAdb):
            def __init__(self):
                super().__init__()
                self.env = 'OLD=1\n'
                self.removed = []
            def shell(self, cmd, timeout=30):
                self.calls.append(cmd)
                if cmd.startswith('cat /sdcard/Download/droiddeck-env'):
                    return self.env
                if 'printf' in cmd and '/sdcard/Download/droiddeck-env' in cmd:
                    self.env = cmd.split('"')[1] if '"' in cmd else ''
                if ' rm -f ' in cmd:
                    self.removed.append(cmd.split(' rm -f ')[1].split()[0])
                return ''
            def sha256(self, path):
                if path.startswith('files/linuxfs/tmp/'):
                    return None          # removed, as teardown intends
                return self.files.get(path)
        adb = EnvAdb()
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, adb, 'c', 41)
        run.begin()
        run.arm_env(adb, ['BL_FRAME_DIGEST=2'])
        self.assertIn('BL_FRAME_DIGEST=2', adb.env)
        m = run.teardown(adb)
        self.assertEqual('ok', m['teardown'])
        self.assertEqual('clean', manifest.trust(m))
        self.assertIn('OLD=1', adb.env)

    def test_teardown_that_finds_something_left_taints_the_run(self):
        class Sticky(FakeAdb):
            def shell(self, cmd, timeout=30):
                self.calls.append(cmd)
                return ''
            def sha256(self, path):
                if path == 'files/linuxfs/tmp/crashcatch.so':
                    return 'sha256:still-here'   # the rm silently failed
                return self.files.get(path)
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, Sticky(), 'c', 41)
        run.begin()
        m = run.teardown(run.adb)
        self.assertEqual('failed', m['teardown'])
        self.assertEqual('tainted', manifest.trust(m))
        self.assertTrue(any('still present' in r for r in m['taint']['reasons']))

    def test_an_armed_run_records_what_it_replaced(self):
        adb = FakeAdb()
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, adb, 'c', 41)
        run.begin()
        run.arm_env(adb, ['BL_FRAME_DIGEST=1'])
        self.assertEqual(['BL_FRAME_DIGEST=1'], run.manifest['env']['armed'])

    def test_teardown_notices_an_env_file_it_could_not_remove(self):
        """The verification must cover the env file, not only the copied libraries."""
        class StuckEnv(FakeAdb):
            def shell(self, cmd, timeout=30):
                self.calls.append(cmd)
                if cmd.startswith('cat /sdcard/Download/droiddeck-env'):
                    return 'BL_FRAME_DIGEST=2\n'   # still there after the rm
                return ''
        run = runs.Run(self.tmp, 'l', 'h', {'v': '1'}, StuckEnv(), 'c', 41)
        run.begin()
        m = run.teardown(run.adb)
        self.assertEqual('failed', m['teardown'])
        self.assertTrue(any('droiddeck-env' in r for r in m['taint']['reasons']))


if __name__ == '__main__':
    unittest.main()
