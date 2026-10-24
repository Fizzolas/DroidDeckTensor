import importlib.util
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('frames', TOOLS / 'diagkit/frames.py')
frames = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frames)


def grid_of(fn):
    return {'w': 16, 'h': 16, 'grid': [[fn(r, c) for c in range(16)] for r in range(16)]}


def rotate(g, quarter_turns):
    out = [row[:] for row in g]
    for _ in range(quarter_turns % 4):
        out = [list(row) for row in zip(*out[::-1])]
    return out


class CorrelateTest(unittest.TestCase):
    def test_an_identical_picture_correlates_at_identity(self):
        a = grid_of(lambda r, c: (r * 16) if (r + c) % 3 else 200)
        result = frames.correlate(a, a)
        self.assertAlmostEqual(1.0, result['r'], places=3)
        self.assertEqual(0, result['quarter_turns'])
        self.assertTrue(result['discriminating'])

    def test_a_rotated_panel_is_found_and_the_reported_turn_reproduces_it(self):
        """The panel is 1080x2404 and the scene 1650x732: one is the other rotated."""
        a = grid_of(lambda r, c: (r * 37 + c * 11) % 256)
        b = dict(a, grid=rotate(a['grid'], 1))
        result = frames.correlate(a, b)
        self.assertAlmostEqual(1.0, result['r'], places=3)
        # The contract: rotating b by the reported turn lines it up with a.
        self.assertEqual(a['grid'], rotate(b['grid'], result['quarter_turns']))

    def test_unrelated_pictures_do_not_correlate(self):
        # Not a ramp: a linear ramp rotated 180 degrees IS its own inverse, which would make an
        # "unrelated" pair correlate perfectly for reasons that have nothing to do with the metric.
        a = grid_of(lambda r, c: (r * 37 + c * 11) % 256)
        b = grid_of(lambda r, c: (r * 5 + c * 91 + 77) % 256)
        result = frames.correlate(a, b)
        self.assertLess(result['r'], 0.8)
        self.assertEqual('differs', result['verdict'])

    def test_two_blank_frames_are_not_evidence(self):
        """The trap: two uniform black frames 'agree' perfectly and prove nothing."""
        black = grid_of(lambda r, c: 0)
        result = frames.correlate(black, black)
        self.assertFalse(result['discriminating'])
        self.assertIn('not discriminating', result['verdict'])

    def test_one_blank_frame_is_not_evidence_either(self):
        black = grid_of(lambda r, c: 0)
        rich = grid_of(lambda r, c: (r * 16 + c) % 256)
        self.assertFalse(frames.correlate(black, rich)['discriminating'])

    def test_a_noise_panel_against_a_real_picture_reports_differs(self):
        import random
        rng = random.Random(7)
        scene = grid_of(lambda r, c: (r * 16 + c) % 256)
        noise = grid_of(lambda r, c: rng.randrange(256))
        result = frames.correlate(scene, noise)
        self.assertLess(abs(result['r']), 0.5)
        self.assertEqual('differs', result['verdict'])

    def test_a_run_missing_its_grid_is_refused(self):
        with self.assertRaises(ValueError):
            frames.correlate({'w': 1, 'h': 1}, {'w': 1, 'h': 1, 'grid': []})


class GridLineTest(unittest.TestCase):
    def chunked(self, grid):
        """The eight lines the compositor emits (512-byte log message limit)."""
        out = []
        for half in range(8):
            flat = [v for row in grid[half * 2:half * 2 + 2] for v in row]
            out.append('[fd] grid%d=' % half + ''.join('%02x' % v for v in flat))
        return out

    def test_the_eight_chunks_assemble_into_the_grid(self):
        cells = [[(r * 16 + c) % 256 for c in range(16)] for r in range(16)]
        self.assertEqual(cells, frames.parse_grid_lines(self.chunked(cells)))

    def test_a_line_without_a_grid_is_none(self):
        self.assertIsNone(frames.parse_grid('[fd] t=1 1650x732 pitch=6600 zero_rows=0 hash=abc'))

    def test_a_truncated_chunk_is_refused(self):
        """Measured on the device: a one-line grid was cut to 506 chars and silently undecodable."""
        self.assertIsNone(frames.parse_grid('[fd] grid0=' + '3' * 100))

    def test_an_incomplete_set_does_not_assemble(self):
        cells = [[(r * 16 + c) % 256 for c in range(16)] for r in range(16)]
        self.assertIsNone(frames.parse_grid_lines(self.chunked(cells)[:7]))

    def test_the_latest_complete_grid_wins(self):
        a = [[(r * 16 + c) % 256 for c in range(16)] for r in range(16)]
        b = [[255 - ((r * 16 + c) % 256) for c in range(16)] for r in range(16)]
        lines = self.chunked(a) + self.chunked(b)
        self.assertEqual(b, frames.parse_grid_lines(lines))

    def test_a_decoded_grid_correlates_with_a_digest_of_the_same_picture(self):
        px = frames.pattern_rgba(320, 200)
        d = frames.digest_rgba(px, 320, 200)
        grid = frames.parse_grid_lines(self.chunked(d['grid']))
        result = frames.correlate({'w': 320, 'h': 200, 'grid': grid}, d)
        self.assertGreater(result['r'], 0.99)


if __name__ == '__main__':
    unittest.main()
