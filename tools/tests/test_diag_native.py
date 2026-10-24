import unittest
from pathlib import Path

RIG = Path(__file__).resolve().parents[2] / 'docs/development/pvr-re/rig'


class CalibratorSourceTest(unittest.TestCase):
    def setUp(self):
        self.src = (RIG / 'calibrate.c').read_text(encoding='utf-8')

    def test_it_supports_the_four_modes(self):
        for mode in ('mmap', 'dlopen', 'crash', 'mark'):
            self.assertIn('"%s"' % mode, self.src)

    def test_the_crash_mode_uses_a_fixed_address_it_also_prints(self):
        self.assertIn('CAL-CRASH', self.src)
        self.assertIn('0xdeadbe0', self.src.lower())

    def test_the_build_script_builds_it(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('calibrate', build)

    def test_it_does_not_require_glibc_newer_than_the_runtime_floor(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('aarch64-linux-gnu.2.31', build)


if __name__ == '__main__':
    unittest.main()
