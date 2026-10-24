"""The two digest producers must agree exactly, or they are two instruments that cannot be compared."""
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'app/src/main/cpp/waylandcomp/src'
spec = importlib.util.spec_from_file_location('frames', ROOT / 'tools/diagkit/frames.py')
frames = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frames)

COMPILER = shutil.which('gcc') or shutil.which('cc') or shutil.which('clang')


@unittest.skipUnless(COMPILER, 'no host C compiler; the parity check needs one')
class ParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        exe = Path(cls.tmp.name) / ('fdselftest.exe' if os.name == 'nt' else 'fdselftest')
        subprocess.run([COMPILER, '-std=c11', '-O1', '-o', str(exe),
                        str(SRC / 'frame_digest.c'), str(SRC / 'frame_digest_selftest.c')],
                       check=True, capture_output=True)
        cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_c(self, data, w, h, pitch, ascii_map=False):
        raw = Path(self.tmp.name) / 'frame.raw'
        raw.write_bytes(data)
        args = [str(self.exe), str(raw), str(w), str(h), str(pitch)]
        if ascii_map:
            args.append('--ascii')
        out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
        return json.loads(out)

    def test_python_and_c_agree_on_a_padded_frame(self):
        w, h, pad = 37, 23, 20
        px = frames.pattern_rgba(w, h)
        rows = [px[y * w * 4:(y + 1) * w * 4] + b'\x11\x22\x33\x44' * (pad // 4) for y in range(h)]
        data = b''.join(rows)
        self.assertEqual(frames.digest_rgba(data, w, h, pitch=w * 4 + pad),
                         self.run_c(data, w, h, w * 4 + pad))

    def test_python_and_c_agree_on_an_ascii_map(self):
        w, h = 64, 40
        data = frames.pattern_rgba(w, h)
        self.assertEqual(frames.digest_rgba(data, w, h, want_ascii=True),
                         self.run_c(data, w, h, w * 4, ascii_map=True))

    def test_python_and_c_agree_on_solid_colour(self):
        w, h = 16, 9
        data = bytes([31, 62, 93, 255]) * (w * h)
        self.assertEqual(frames.digest_rgba(data, w, h), self.run_c(data, w, h, w * 4))

    def test_python_and_c_agree_on_zero_rows(self):
        w, h = 8, 12
        rows = [bytes([7, 7, 7, 255]) * w for _ in range(h)]
        for i in (3, 4, 5, 10):
            rows[i] = bytes(4 * w)
        data = b''.join(rows)
        d = frames.digest_rgba(data, w, h)
        self.assertEqual([[3, 3], [10, 1]], d['zero_runs'])
        self.assertEqual(d, self.run_c(data, w, h, w * 4))

    def test_the_c_core_refuses_a_short_buffer(self):
        raw = Path(self.tmp.name) / 'short.raw'
        raw.write_bytes(b'\x00' * 16)
        result = subprocess.run([str(self.exe), str(raw), '8', '8', '32'], capture_output=True)
        self.assertNotEqual(0, result.returncode)


if __name__ == '__main__':
    unittest.main()
