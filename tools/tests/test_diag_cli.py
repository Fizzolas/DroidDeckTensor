import contextlib
import importlib.util
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('cli', TOOLS / 'diagkit/cli.py')
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class CliTest(unittest.TestCase):
    def test_every_documented_command_exists(self):
        parser = cli.build_parser()
        actions = list(parser._subparsers._group_actions[0].choices)
        for name in ('doctor', 'run', 'prior', 'analyze', 'compare', 'frame', 'export'):
            self.assertIn(name, actions)

    def test_run_requires_a_label_and_an_arm(self):
        parser = cli.build_parser()
        with self.assertRaises(SystemExit):
            parser.parse_args(['run', '--label', 'x'])
        ns = parser.parse_args(['run', '--label', 'x', '--arm', 'icd=A', '--hypothesis', 'h'])
        self.assertEqual({'icd': 'A'}, cli._arm(ns.arm))

    def test_usage_errors_exit_2(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(2, cli.main(['nonesuch']))


class FrameCommandTest(unittest.TestCase):
    def digest(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
            code = cli.main(argv)
        return code, json.loads(buf.getvalue())

    def test_a_raw_file_is_digested(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'f.raw'
            w, h = 4, 4
            p.write_bytes(bytes([10, 20, 30, 255]) * (w * h))
            code, d = self.digest(['frame', '--raw', str(p), '--width', str(w), '--height', str(h)])
            self.assertEqual(0, code)
            self.assertEqual((w, h), (d['w'], d['h']))
            self.assertEqual(0, d['zero_rows'])
            self.assertEqual('file', d['source'])

    def test_an_explicit_pitch_is_honoured(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'f.raw'
            w, h, pitch = 4, 3, 32
            rows = [bytes([1, 2, 3, 255]) * w + b'\xff' * (pitch - w * 4) for _ in range(h)]
            p.write_bytes(b''.join(rows))
            code, d = self.digest(['frame', '--raw', str(p), '--width', str(w),
                                   '--height', str(h), '--pitch', str(pitch)])
            self.assertEqual(0, code)
            self.assertEqual(0, d['zero_rows'])

    def test_ascii_is_only_emitted_when_asked(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / 'f.raw'
            p.write_bytes(bytes([9, 9, 9, 255]) * 16)
            _, plain = self.digest(['frame', '--raw', str(p), '--width', '4', '--height', '4'])
            _, mapped = self.digest(['frame', '--raw', str(p), '--width', '4', '--height', '4', '--ascii'])
            self.assertNotIn('ascii', plain)
            self.assertIn('ascii', mapped)

    def test_a_panel_digest_decodes_the_devices_own_header(self):
        """screencap is a declared data source; its bytes are decoded and never stored."""
        w, h = 4, 2
        raw = struct.pack('<III', w, h, 1) + bytes([5, 6, 7, 255]) * (w * h)

        class FakeDevice:
            def screencap_raw(self):
                return raw

        original = cli._adb
        cli._adb = lambda args: FakeDevice()
        try:
            code, d = self.digest(['frame', '--panel'])
        finally:
            cli._adb = original
        self.assertEqual(0, code)
        self.assertEqual('panel', d['source'])
        self.assertEqual((w, h), (d['w'], d['h']))


if __name__ == '__main__':
    unittest.main()
