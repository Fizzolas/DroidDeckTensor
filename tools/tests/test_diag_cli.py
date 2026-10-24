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


class RunWiringTest(unittest.TestCase):
    """The run command must refuse to go on when a calibration fails."""

    def fake(self):
        class FakeDevice:
            def sha256(self, path):
                return None
            def provider(self, method, arg=None):
                if method == 'state':
                    return {'appVersion': '0.3.4'}
                if method == 'mark':
                    return {'ok': True}
                if method == 'snapshot':
                    return {'ok': True, 'dir': 'files/linuxfs/tmp/snapshot-x'}
                return {'ok': True}
            def pull(self, remote, local):
                Path(local).mkdir(parents=True, exist_ok=True)
                (Path(local) / 'procs.txt').write_text('pid=1\n')
                return Path(local)
        return FakeDevice()

    def invoke(self, argv):
        original = cli._adb
        cli._adb = lambda args: self.fake()
        try:
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
                code = cli.main(argv)
            return code, json.loads(buf.getvalue().splitlines()[-1])
        finally:
            cli._adb = original

    def test_a_passing_calibration_lets_the_run_proceed(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = self.invoke(['run', '--label', 'display', '--hypothesis', 'h',
                                     '--arm', 'icd=A', '--root', tmp,
                                     '--calibrate', 'frame-digest'])
            self.assertEqual(0, code)
            self.assertEqual(['frame-digest'], out['calibrated'])

    def test_a_failing_calibration_refuses_the_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = self.invoke(['run', '--label', 'display', '--hypothesis', 'h',
                                     '--arm', 'icd=A', '--root', tmp,
                                     '--calibrate', 'no-such-instrument'])
            self.assertEqual(4, code)
            self.assertFalse(out['ok'])
            self.assertIn('calibration failed', out['refused'])

    def test_a_snapshot_is_taken_when_asked(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out = self.invoke(['run', '--label', 'display', '--hypothesis', 'h',
                                     '--arm', 'icd=A', '--root', tmp, '--snapshot'])
            self.assertEqual(0, code)
            self.assertIsNotNone(out['snapshot'])

    def test_a_dirty_device_refuses_before_measuring(self):
        class Dirty(self.fake().__class__):
            pass
        with tempfile.TemporaryDirectory() as tmp:
            original = cli._adb
            class Dev(Dirty):
                def sha256(self, path):
                    return 'sha256:leftover'
            cli._adb = lambda args: Dev()
            try:
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
                    code = cli.main(['run', '--label', 'l', '--hypothesis', 'h',
                                     '--arm', 'a=b', '--root', tmp])
            finally:
                cli._adb = original
            self.assertEqual(4, code)


class ExportTest(unittest.TestCase):
    def test_export_writes_text_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            run.mkdir()
            (run / 'manifest.json').write_text(json.dumps(
                {'schema': 1, 'arm': {}, 'instruments': {}, 'taint': {'reasons': []}}))
            (run / 'wayland.log').write_text('line\n')
            (run / 'frame.raw').write_bytes(b'\x00' * 64)
            (run / 'libgbm.so').write_bytes(b'\x7fELF')
            dest = Path(tmp) / 'out'
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(0, cli.main(['export', str(run), str(dest)]))
            names = {p.name for p in dest.rglob('*') if p.is_file()}
            self.assertIn('wayland.log', names)
            self.assertNotIn('frame.raw', names)
            self.assertNotIn('libgbm.so', names)
            self.assertIn('findings.txt', names)

    def test_an_exported_bundle_carries_the_citations(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp) / 'run'
            run.mkdir()
            (run / 'manifest.json').write_text(json.dumps(
                {'schema': 1, 'arm': {}, 'instruments': {}, 'taint': {'reasons': []}}))
            (run / 'wayland.log').write_text(
                '00:00:01.000  fd   t=1.000 8x8 pitch=32 zero_rows=0 hash=aa\n')
            dest = Path(tmp) / 'out'
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                cli.main(['export', str(run), str(dest)])
            self.assertIn('wayland.log:1', (dest / 'findings.txt').read_text())


if __name__ == '__main__':
    unittest.main()
