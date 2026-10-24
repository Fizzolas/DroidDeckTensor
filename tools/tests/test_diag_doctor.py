import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


doctor = load('doctor')


class FakeAdb:
    """Stands in for a device: a path map and a hash per path."""
    def __init__(self, files):
        self.files = files
        self.calls = []

    def shell(self, cmd, timeout=30):
        self.calls.append(cmd)
        return ''

    def ls(self, path):
        return sorted(p for p in self.files if p.startswith(path))

    def sha256(self, path):
        return self.files.get(path)

    def stat_size(self, path):
        return None


class DoctorTest(unittest.TestCase):
    def test_a_stock_device_is_clean(self):
        inv = doctor.inventory(FakeAdb({}))
        state, reasons = doctor.verdict(inv)
        self.assertEqual('clean', state)
        self.assertEqual([], reasons)

    def test_an_instrument_left_behind_is_dirty(self):
        adb = FakeAdb({doctor.ARTIFACTS[0]['app_path']: 'sha256:leftover'})
        state, reasons = doctor.verdict(doctor.inventory(adb))
        self.assertEqual('dirty', state)
        self.assertIn(doctor.ARTIFACTS[0]['name'], reasons[0])

    def test_a_tracked_file_restored_to_its_stock_hash_is_clean(self):
        tracked = dict(doctor.ARTIFACTS[0], stock='sha256:stock')
        entries = [{'name': tracked['name'], 'path': tracked['app_path'], 'present': True,
                    'sha256': 'sha256:stock', 'stock': tracked['stock']}]
        self.assertEqual('clean', doctor.verdict(entries)[0])

    def test_a_tracked_file_with_another_hash_is_dirty(self):
        tracked = dict(doctor.ARTIFACTS[0], stock='sha256:stock')
        entries = [{'name': tracked['name'], 'path': tracked['app_path'], 'present': True,
                    'sha256': 'sha256:changed', 'stock': tracked['stock']}]
        self.assertEqual('dirty', doctor.verdict(entries)[0])

    def test_the_env_file_is_part_of_the_inventory(self):
        names = {a['name'] for a in doctor.ARTIFACTS}
        self.assertIn('droiddeck-env', names)

    def test_inventory_reports_what_it_found(self):
        a = doctor.ARTIFACTS[0]
        adb = FakeAdb({a['app_path']: 'sha256:x'})
        entry = next(e for e in doctor.inventory(adb) if e['name'] == a['name'])
        self.assertTrue(entry['present'])
        self.assertEqual('sha256:x', entry['sha256'])


if __name__ == '__main__':
    unittest.main()
