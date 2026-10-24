import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('registry', TOOLS / 'diagkit/registry.py')
registry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(registry)


class RegistryTest(unittest.TestCase):
    def test_the_shipped_registry_loads_and_is_valid(self):
        reg = registry.load()
        self.assertEqual([], registry.validate(reg))
        for required in ('crashcatch', 'mmwatch', 'symwatch', 'frame-digest', 'panel-digest', 'snapshot'):
            self.assertIn(required, reg)

    def test_every_entry_declares_all_five_fields(self):
        for name, entry in registry.load().items():
            for field in ('kind', 'build', 'identity', 'calibration', 'cost'):
                self.assertIn(field, entry, f'{name} is missing {field}')

    def test_each_defect_is_reported_not_crashed(self):
        bad = {'x': {'kind': 'host-tool', 'build': '', 'identity': '', 'calibration': ''}}
        self.assertEqual(["x: missing 'cost'", "x: unknown calibration ''"], registry.validate(bad))

    def test_an_empty_calibration_is_a_defect_on_its_own(self):
        """Absent or blank both mean 'never checked', which must never pass silently."""
        bad = {'x': {'kind': 'host-tool', 'build': 'b', 'identity': 'i', 'calibration': '', 'cost': 0}}
        self.assertEqual(["x: unknown calibration ''"], registry.validate(bad))

    def test_a_calibration_that_is_not_a_known_check_is_rejected(self):
        bad = {'x': {'kind': 'host-tool', 'build': 'b', 'identity': 'i', 'calibration': 'vibes', 'cost': 0}}
        errors = registry.validate(bad)
        self.assertTrue(errors and 'unknown calibration' in errors[0])

    def test_cost_sums_over_the_named_instruments(self):
        reg = registry.load()
        mine = ['crashcatch', 'mmwatch']
        self.assertEqual(sum(reg[n]['cost'] for n in mine), registry.cost_per_minute(reg, mine))

    def test_an_unknown_name_costs_nothing_and_is_ignored(self):
        self.assertEqual(0, registry.cost_per_minute(registry.load(), ['nope']))


if __name__ == '__main__':
    unittest.main()
