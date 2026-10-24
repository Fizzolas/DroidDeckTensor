import importlib.util
import json
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


compare = load('compare')
manifest = load('manifest')
runs = load('runs')


def run(tmp, name, arm, instruments=None):
    d = Path(tmp) / name
    d.mkdir(parents=True)
    m = manifest.new_manifest(name, 'h', arm, 'c', 41)
    for k, ok in (instruments or {}).items():
        manifest.stamp_instrument(m, k, 'id', ok, 'd')
    (d / 'manifest.json').write_text(json.dumps(m))
    return d


class CompareTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_two_runs_differing_in_one_declared_field_are_comparable(self):
        a = run(self.tmp, 'a', {'icd': 'A', 'shim': 'nulldev'}, {'frame-digest': True})
        b = run(self.tmp, 'b', {'icd': 'B', 'shim': 'nulldev'}, {'frame-digest': True})
        self.assertEqual([], compare.refusal(a, b))

    def test_two_differences_are_refused_with_both_named(self):
        a = run(self.tmp, 'a', {'icd': 'A', 'shim': 'nulldev'})
        b = run(self.tmp, 'b', {'icd': 'B', 'shim': 'ahb'})
        reasons = compare.refusal(a, b)
        self.assertEqual(1, len(reasons))
        self.assertIn('shim', reasons[0])
        self.assertIn('icd', reasons[0])

    def test_a_differing_instrument_calibration_is_refused(self):
        """§63: an arm that also changed the logging looked like one variable and was not."""
        a = run(self.tmp, 'a', {'icd': 'A'}, {'frame-digest': True})
        b = run(self.tmp, 'b', {'icd': 'B'}, {'frame-digest': False})
        self.assertIn('frame-digest', compare.refusal(a, b)[0])

    def test_a_tainted_arm_is_refused(self):
        a = run(self.tmp, 'a', {'icd': 'A'})
        b = run(self.tmp, 'b', {'icd': 'B'})
        m = json.loads((b / 'manifest.json').read_text())
        manifest.mark_tainted(m, 'debris')
        (b / 'manifest.json').write_text(json.dumps(m))
        self.assertTrue(any('tainted' in r for r in compare.refusal(a, b)))

    def test_identical_arms_are_refused_because_nothing_varies(self):
        a = run(self.tmp, 'a', {'icd': 'A'})
        b = run(self.tmp, 'b', {'icd': 'A'})
        self.assertTrue(any('exactly one' in r for r in compare.refusal(a, b)))

    def test_digest_diff_reports_which_fields_moved(self):
        a = {'zero_rows': 0, 'hash': 'sha256:a', 'grid': [[1] * 16] * 16}
        b = {'zero_rows': 12, 'hash': 'sha256:b', 'grid': [[1] * 16] * 16}
        moved = compare.diff_digests(a, b)
        self.assertIn('zero_rows', moved)
        self.assertIn('hash', moved)
        self.assertNotIn('grid', moved)

    def test_arm_differs_agrees_with_the_run_module(self):
        self.assertEqual(['value'], runs.arm_differs({'value': 'A'}, {'value': 'B'}))


if __name__ == '__main__':
    unittest.main()
