import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frames', TOOLS / 'diagkit/frames.py')
frames = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frames)


def solid(w, h, rgba):
    return bytes(rgba) * (w * h)


class DigestTest(unittest.TestCase):
    def test_a_solid_frame_has_one_band_mean(self):
        d = frames.digest_rgba(solid(8, 8, (0x33, 0x33, 0x33, 0xff)), 8, 8)
        means = {b[0] for b in d['bands']}
        self.assertEqual(1, len(means))
        self.assertEqual((77 * 0x33 + 150 * 0x33 + 29 * 0x33) >> 8, means.pop())

    def test_a_tightly_packed_frame_and_a_padded_one_digest_identically(self):
        """The defect that cost §65-68: a producer's real row pitch is not width*4."""
        w, h, padded_pitch = 8, 4, 32
        rows = [bytes([i * 4, 0, 0, 255]) * w for i in range(h)]
        tight = b''.join(rows)
        padded = b''.join(r + b'\xde\xad\xbe\xef' * ((padded_pitch - w * 4) // 4) for r in rows)
        self.assertEqual(frames.digest_rgba(tight, w, h, pitch=w * 4),
                         frames.digest_rgba(padded, w, h, pitch=padded_pitch))

    def test_a_zero_row_is_reported_with_its_position_and_length(self):
        w, h = 4, 10
        rows = [bytes([9, 9, 9, 255]) * w for _ in range(h)]
        for i in (2, 3, 7):
            rows[i] = bytes(4 * w)
        d = frames.digest_rgba(b''.join(rows), w, h)
        self.assertEqual(3, d['zero_rows'])
        self.assertEqual([[2, 2], [7, 1]], d['zero_runs'])

    def test_the_hash_changes_when_a_pixel_changes(self):
        a = frames.digest_rgba(solid(8, 8, (1, 2, 3, 4)), 8, 8)
        b = frames.digest_rgba(solid(8, 8, (1, 2, 3, 5)), 8, 8)
        self.assertNotEqual(a['hash'], b['hash'])

    def test_the_ascii_map_is_the_requested_shape_and_is_monotonic(self):
        w, h = 64, 40
        px = bytearray()
        for y in range(h):
            v = int(255 * y / (h - 1))
            px += bytes([v, v, v, 255]) * w
        d = frames.digest_rgba(bytes(px), w, h, want_ascii=True)
        lines = d['ascii'].splitlines()
        self.assertEqual(20, len(lines))
        self.assertTrue(all(len(line) == 48 for line in lines))
        self.assertLess(frames.RAMP.index(lines[0][0]), frames.RAMP.index(lines[-1][0]))

    def test_a_truncated_buffer_is_refused_rather_than_guessed(self):
        with self.assertRaises(ValueError):
            frames.digest_rgba(b'\x00' * 10, 8, 8)

    def test_screencap_raw_is_decoded_by_its_own_header(self):
        import struct
        w, h = 3, 2
        raw = struct.pack('<III', w, h, 1) + bytes([1, 2, 3, 4]) * (w * h)
        px, pw, ph, fmt = frames.parse_screencap(raw)
        self.assertEqual((w, h, 1), (pw, ph, fmt))
        self.assertEqual(bytes([1, 2, 3, 4]) * 6, px)


if __name__ == '__main__':
    unittest.main()
