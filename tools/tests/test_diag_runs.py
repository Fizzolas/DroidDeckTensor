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


if __name__ == '__main__':
    unittest.main()
