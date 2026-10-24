import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('manifest', TOOLS / 'diagkit/manifest.py')
manifest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manifest)


class ManifestTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.m = manifest.new_manifest(
            'display path', 'the layer path presents', {'name': 'icd', 'value': 'A'}, 'abc123', 40)

    def test_a_new_manifest_is_clean(self):
        self.assertEqual('clean', manifest.trust(self.m))

    def test_a_tainted_run_is_not_clean(self):
        manifest.mark_tainted(self.m, 'shim left on device')
        self.assertEqual('tainted', manifest.trust(self.m))
        self.assertEqual(['shim left on device'], self.m['taint']['reasons'])

    def test_an_instrument_must_have_passed_its_calibration_to_be_trusted(self):
        manifest.stamp_instrument(self.m, 'crashcatch', 'sha256:aa', False, 'known address not reported')
        self.assertFalse(manifest.instrument_trusted(self.m, 'crashcatch'))
        manifest.stamp_instrument(self.m, 'crashcatch', 'sha256:aa', True, 'ok')
        self.assertTrue(manifest.instrument_trusted(self.m, 'crashcatch'))

    def test_an_instrument_never_stamped_is_not_trusted(self):
        self.assertFalse(manifest.instrument_trusted(self.m, 'mmwatch'))

    def test_a_deploy_whose_pushed_hash_differs_taints_the_run(self):
        manifest.stamp_deploy(self.m, 'files/tmp/crashcatch.so', 'sha256:aa', 'sha256:bb')
        self.assertEqual('tainted', manifest.trust(self.m))
        self.assertIn('files/tmp/crashcatch.so', self.m['taint']['reasons'][0])

    def test_a_matching_deploy_does_not_taint(self):
        manifest.stamp_deploy(self.m, 'files/tmp/crashcatch.so', 'sha256:aa', 'sha256:aa')
        self.assertEqual('clean', manifest.trust(self.m))

    def test_a_manifest_round_trips_through_disk(self):
        manifest.stamp_instrument(self.m, 'frame-digest', 'sha256:cc', True, 'ok')
        path = manifest.write_manifest(self.tmp, self.m)
        self.assertTrue(path.is_file())
        self.assertEqual(self.m, manifest.read_manifest(self.tmp))

    def test_hashing_matches_hashlib(self):
        f = self.tmp / 'x'
        f.write_bytes(b'hello')
        self.assertEqual('sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824',
                         manifest.sha256_file(f))


if __name__ == '__main__':
    unittest.main()
