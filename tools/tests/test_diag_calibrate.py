import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


frames = load('frames')
calibrate = load('calibrate')


def digest_of_pattern(w, h, **kw):
    return frames.digest_rgba(frames.pattern_rgba(w, h), w, h, **kw)


class DigestCalibrationTest(unittest.TestCase):
    def test_the_pattern_is_recovered(self):
        ok, detail = calibrate.check_digest(digest_of_pattern(320, 200), 320, 200)
        self.assertTrue(ok, detail)

    def test_a_digest_of_the_wrong_extent_is_refused(self):
        """A helper that read a 2404x1080 frame as 256x256 looked exactly like this."""
        d = digest_of_pattern(320, 200)
        ok, detail = calibrate.check_digest(d, 256, 256)
        self.assertFalse(ok)
        self.assertIn('extent', detail.lower())

    def test_a_padded_read_that_honours_pitch_passes(self):
        w, h = 320, 200
        tight = frames.pattern_rgba(w, h)
        rows = [tight[y * w * 4:(y + 1) * w * 4] for y in range(h)]
        padded = b''.join(r + b'\x00\x00\x00\x00' * 16 for r in rows)
        d = frames.digest_rgba(padded, w, h, pitch=w * 4 + 64)
        ok, detail = calibrate.check_digest(d, w, h)
        self.assertTrue(ok, detail)

    def test_a_uniform_frame_fails_because_the_block_is_missing(self):
        w, h = 320, 200
        d = frames.digest_rgba(bytes([128, 128, 128, 255]) * (w * h), w, h)
        ok, detail = calibrate.check_digest(d, w, h)
        self.assertFalse(ok)
        self.assertIn('pattern', detail.lower())

    def test_a_crash_report_is_checked_for_the_known_address(self):
        ok, _ = calibrate.check_crash_report('si_addr=0xdeadbe0  tid=7', '0xdeadbe0')
        self.assertTrue(ok)
        ok, detail = calibrate.check_crash_report('si_addr=0x1  tid=7', '0xdeadbe0')
        self.assertFalse(ok)
        self.assertIn('0xdeadbe0', detail)

    def test_marks_must_all_appear(self):
        ok, _ = calibrate.check_marks('a\nMARK-x\nb\n', ['MARK-x'])
        self.assertTrue(ok)
        ok, detail = calibrate.check_marks('nothing here\n', ['MARK-x'])
        self.assertFalse(ok)
        self.assertIn('MARK-x', detail)


if __name__ == '__main__':
    unittest.main()
