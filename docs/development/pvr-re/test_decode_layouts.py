"""Offline regressions. Run: python -m unittest discover -s docs/development/pvr-re -v

Uses a tiny synthetic vendor disassembly, never adb or a connected device.
The real saved-capture byte comparison is run separately with PVR_VENDOR.
"""
import csv
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
OUTPUTS = ["bridge_table.csv", "bringup_layouts.csv", "submit_layouts.csv",
           "dispatch_layouts.csv", "decode_out.txt", "bridge_field_map.md"]
VALID = "B bridge=1 func=0 ret=0 errno=2 in[1]=aa out[1]=bb"
OTHER = "B bridge=2 func=0 ret=0 errno=0 in=NULL out[1]=00"


def load_decoder(root):
    spec = importlib.util.spec_from_file_location("decoder_under_test", root / "decode_layouts.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RecordTests(unittest.TestCase):
    def setUp(self):
        self.decoder = load_decoder(ROOT)
        self.rx = self.decoder.log_regexes()

    def test_valid_records_and_ignored_other_ioctl(self):
        rec = self.decoder.parse_bridge_line(VALID, self.rx)
        self.assertEqual(rec["in"], ("1", "aa"))
        self.assertEqual(rec["out"], ("1", "bb"))
        self.assertEqual(self.decoder.parse_bridge_line(OTHER, self.rx)["in"], ("0", ""))
        self.assertIsNone(self.decoder.parse_bridge_line("D req=0xc0406400 ret=0 errno=2", self.rx))
        self.assertIsNone(self.decoder.parse_bridge_line("", self.rx))

    def test_malformed_records_are_rejected(self):
        lines = [
            "garbage " + VALID, VALID + "zz", VALID + " out[1]=cc",
            VALID.replace("out[1]=bb", "out[4]=bb"),
            VALID.replace("in[1]=aa", "in[4]=aa"),
            VALID.replace("out[1]=bb", "out[1]=b"),
            VALID.replace("out[1]=bb", "out[1]=gg"),
            VALID.replace("out[1]=bb", "out[1]=bbbb"),
            VALID.split(" out")[0],
            VALID.replace("in[1]=aa out[1]=bb", "out[1]=bb in[1]=aa"),
            VALID.replace("bridge=1", "bridge=4294967296"),
            VALID.replace("errno=2", "errno=2147483648"),
            "B broken", "D broken", "unexpected output",
        ]
        for line in lines:
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.decoder.parse_bridge_line(line, self.rx)

    def test_all_saved_capture_records_remain_valid(self):
        expected_counts = {"bringup": 180, "fill": 186, "submit": 183, "dispatch": 191}
        for phase, expected in expected_counts.items():
            with self.subTest(phase=phase):
                records = [self.decoder.parse_bridge_line(line, self.rx)
                           for line in (ROOT / f"bridge_{phase}.log").read_text().splitlines()]
                self.assertEqual(sum(rec is not None for rec in records), expected)

    def test_log_formats_still_come_from_shared_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            header = Path(tmp) / "bridge_abi.h"
            text = self.decoder.ABI.read_text().replace(
                "B bridge=%u func=%u ret=%d errno=%d", "B bid=%u fid=%u rc=%d err=%d"
            ).replace(" %s[%u]=", " %s(%u):")
            header.write_text(text)
            self.decoder.ABI = header
            rx = self.decoder.log_regexes()
            rec = self.decoder.parse_bridge_line("B bid=1 fid=0 rc=0 err=2 in(1):aa out(1):bb", rx)
            self.assertEqual(rec["bridge"], 1)
            self.assertEqual(rec["out"], ("1", "bb"))


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.pipeline = self.work / "pipeline"
        self.pipeline.mkdir()
        (self.pipeline / "hooks").mkdir()
        for name in ["decode_layouts.py", "extract_bridge.py", "run_pipeline.sh"]:
            shutil.copy2(ROOT / name, self.pipeline / name)
        shutil.copy2(ROOT / "hooks/bridge_abi.h", self.pipeline / "hooks/bridge_abi.h")
        self.vendor = self.work / "vendor"
        self.vendor.mkdir()
        (self.vendor / "syms.txt").write_text("1000 BridgeConnect\n2000 BridgeOther\n3000 End\n")
        lines = []
        for base, bridge, size in [(0x1000, 1, 1), (0x2000, 2, 0)]:
            instructions = [f"mov w1, #{bridge}", "mov w2, #0", "mov x3, sp",
                            f"mov w4, #{size}", "add x5, sp, #8", "mov w6, #1",
                            "bl 37a70 <PVRSRVBridgeCall>", "ret"]
            lines.extend(f" {base + 4 * i:x}:\t{op}" for i, op in enumerate(instructions))
        (self.vendor / "srv_um.dis").write_text("\n".join(lines) + "\n")
        self.env = os.environ.copy()
        self.env.update(PVR_VENDOR=self.vendor.as_posix(), PYTHON=Path(sys.executable).as_posix(),
                        PYTHONDONTWRITEBYTECODE="1")
        # Tests always use their own logs, regardless of the caller's environment.
        self.env.pop("LOG_DIR", None)
        self.env.pop("MSYS_NO_PATHCONV", None)
        for phase in ["bringup", "fill", "dispatch"]:
            text = VALID + "\n" + (OTHER + "\n" if phase == "dispatch" else "")
            (self.pipeline / f"bridge_{phase}.log").write_text(text)
        self.assertEqual(self.run_script("extract_bridge.py").returncode, 0)
        for phase, capture in [("bringup", "bringup_layouts.csv"), ("fill", "submit_layouts.csv"),
                               ("dispatch", "dispatch_layouts.csv")]:
            result = self.from_log(self.pipeline / f"bridge_{phase}.log", self.pipeline / capture,
                                   "trace" if phase == "bringup" else "sorted")
            self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_script("decode_layouts.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        (self.pipeline / "decode_out.txt").write_bytes(result.stdout)
        self.baseline = {name: (self.pipeline / name).read_bytes() for name in OUTPUTS}

    def run_script(self, script, *args):
        return subprocess.run([sys.executable, str(self.pipeline / script), *map(str, args)],
                              env=self.env, capture_output=True, timeout=20)

    def from_log(self, log, output, order="sorted"):
        return self.run_script("decode_layouts.py", "--from-log", log, output, order)

    def change_csv(self, filename, mutate):
        path = self.pipeline / filename
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            fields = reader.fieldnames
            rows = list(reader)
        mutate(rows)
        with path.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def assert_verification_fails(self, message):
        result = self.run_script("decode_layouts.py")
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn(message, result.stderr.decode())
        self.assertNotIn(b"failures: none", result.stdout)
        self.assertEqual((self.pipeline / "bridge_field_map.md").read_bytes(),
                         self.baseline["bridge_field_map.md"])

    def test_bad_log_cli_does_not_replace_output(self):
        bad_logs = ["", "\n", "D req=0xc0406400 ret=0 errno=2\n", "B broken\n",
                    VALID + "\nB broken\n", "garbage " + VALID + "\n",
                    VALID.replace("out[1]=bb", "out[4]=bb") + "\n",
                    VALID.replace("ret=0", "ret=-1") + "\n",
                    VALID + "\n" + VALID.replace("ret=0", "ret=-1") + "\n",
                    VALID + "\n" + VALID.replace("in[1]=aa", "in[2]=aabb") + "\n",
                    VALID.replace("bridge=1", "bridge=999") + "\n"]
        log = self.work / "bad.log"
        output = self.work / "result.csv"
        for text in bad_logs:
            with self.subTest(text=text):
                log.write_text(text)
                output.write_bytes(b"existing output")
                result = self.from_log(log, output)
                self.assertNotEqual(result.returncode, 0, result.stdout.decode())
                self.assertTrue(result.stderr)
                self.assertEqual(output.read_bytes(), b"existing output")

    def test_valid_repeated_samples_keep_first_hex_and_count(self):
        log = self.work / "repeated.log"
        log.write_text(VALID + "\n" + VALID.replace("in[1]=aa", "in[1]=cc") + "\n")
        output = self.work / "result.csv"
        result = self.from_log(log, output)
        self.assertEqual(result.returncode, 0, result.stderr)
        with output.open() as f:
            row = next(csv.DictReader(f))
        self.assertEqual((row["calls"], row["in_hex"]), ("2", "aa"))

    def test_first_phase_size_mismatch_fails(self):
        self.change_csv("bringup_layouts.csv", lambda rows: rows[0].update(in_size="2", in_hex="aabb"))
        self.assert_verification_fails("in size")

    def test_later_phase_size_mismatch_fails(self):
        self.change_csv("submit_layouts.csv", lambda rows: rows[0].update(out_size="2", out_hex="bbcc"))
        self.assert_verification_fails("out size")

    def test_later_phase_bridge_id_mismatch_fails(self):
        self.change_csv("submit_layouts.csv", lambda rows: rows[0].update(bridge="99"))
        self.assert_verification_fails("bridge/func")

    def test_unknown_wrapper_fails(self):
        self.change_csv("bringup_layouts.csv", lambda rows: rows[0].update(symbol="AuditMissingWrapper"))
        self.assert_verification_fails("AuditMissingWrapper")

    def test_missing_call_site_fails(self):
        path = self.vendor / "srv_um.dis"
        path.write_text(path.read_text().replace("bl 37a70 <PVRSRVBridgeCall>", "nop", 1))
        self.assert_verification_fails("NO CALL SITE")

    def test_empty_capture_csv_fails(self):
        self.change_csv("submit_layouts.csv", lambda rows: rows.clear())
        self.assert_verification_fails("empty capture")

    def test_invalid_csv_rows_fail(self):
        mutations = [dict(in_size="-1"), dict(in_hex=""), dict(out_hex="gg"), dict(calls="0"),
                     dict(ret="-1"), dict(bridge="4294967296"), dict(symbol="")]
        path = self.pipeline / "submit_layouts.csv"
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                path.write_bytes(self.baseline[path.name])
                self.change_csv(path.name, lambda rows: rows[0].update(mutation))
                result = self.run_script("decode_layouts.py")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"submit_layouts.csv:2", result.stderr)

    def test_duplicate_csv_row_fails(self):
        self.change_csv("submit_layouts.csv", lambda rows: rows.append(rows[0].copy()))
        self.assert_verification_fails("duplicate")

    def test_wrong_csv_schema_fails(self):
        path = self.pipeline / "submit_layouts.csv"
        path.write_text(path.read_text().replace("out_hex", "unexpected", 1))
        self.assert_verification_fails("columns")

    def test_extra_and_missing_csv_cells_fail(self):
        path = self.pipeline / "submit_layouts.csv"
        for cells in ["1,0,BridgeConnect,1,0,1,aa,1,bb,extra", "1,0,BridgeConnect,1,0,1,aa,1"]:
            with self.subTest(cells=cells):
                path.write_text(self.baseline[path.name].decode().splitlines()[0] + "\n" + cells + "\n")
                self.assert_verification_fails("columns")

    def run_pipeline(self, *args):
        # Git Bash is the supported shell on Windows; subprocess needs its native executable path.
        bash = shutil.which("bash")
        if sys.platform == "win32" and Path("C:/Program Files/Git/usr/bin/bash.exe").is_file():
            bash = "C:/Program Files/Git/usr/bin/bash.exe"
        self.assertIsNotNone(bash, "pipeline tests require Bash")
        return subprocess.run([bash, (self.pipeline / "run_pipeline.sh").as_posix(), *args],
                              env=self.env, capture_output=True, timeout=30)

    def test_valid_pipeline_keeps_all_six_outputs_identical(self):
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"checked 2 functions; failures: none", result.stdout)
        for name, expected in self.baseline.items():
            with self.subTest(output=name):
                self.assertEqual((self.pipeline / name).read_bytes(), expected)

    def test_invalid_pipeline_log_fails_without_publishing(self):
        log = self.pipeline / "bridge_dispatch.log"
        for text in ["", "B broken\n", VALID.replace("out[1]=bb", "out[4]=bb") + "\n",
                     VALID.replace("out[1]=bb", "out[2]=bbcc") + "\n"]:
            with self.subTest(text=text):
                log.write_text(text)
                result = self.run_pipeline()
                self.assertNotEqual(result.returncode, 0, result.stdout.decode())
                self.assertNotIn(b"failures: none", result.stdout)
                for name, expected in self.baseline.items():
                    self.assertEqual((self.pipeline / name).read_bytes(), expected, name)

    def test_each_phase_requires_its_reference_calls(self):
        # Keep every function present somewhere else: union-only validation is insufficient.
        for phase in ["bringup", "fill", "dispatch"]:
            path = self.pipeline / f"bridge_{phase}.log"
            saved = path.read_bytes()
            reference = self.pipeline / ("bringup_layouts.csv" if phase == "bringup" else
                                         "submit_layouts.csv" if phase == "fill" else "dispatch_layouts.csv")
            reference_saved = reference.read_bytes()
            if phase != "dispatch":
                self.change_csv(reference.name, lambda rows: rows.append({
                    "bridge": "2", "func": "0", "symbol": "BridgeOther", "calls": "1", "ret": "0",
                    "in_size": "0", "in_hex": "", "out_size": "1", "out_hex": "00"}))
            try:
                expected_outputs = {name: (self.pipeline / name).read_bytes() for name in OUTPUTS}
                path.write_text(VALID + "\n")
                result = self.run_pipeline()
                with self.subTest(phase=phase):
                    self.assertNotEqual(result.returncode, 0, result.stdout.decode())
                    self.assertIn(phase.encode(), result.stderr)
                    self.assertIn(b"structure", result.stderr)
                    self.assertNotIn(b"failures: none", result.stdout)
                    for name, expected in expected_outputs.items():
                        self.assertEqual((self.pipeline / name).read_bytes(), expected, name)
            finally:
                path.write_bytes(saved)
                reference.write_bytes(reference_saved)

    def test_generic_from_log_allows_partial_trace(self):
        log = self.work / "connect-only.log"
        log.write_text(VALID + "\n")
        out = self.work / "partial.csv"
        result = self.from_log(log, out)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        with out.open() as f:
            self.assertEqual([row["symbol"] for row in csv.DictReader(f)], ["BridgeConnect"])

    def test_pipeline_ignores_run_specific_bytes_counts_and_order(self):
        log = self.pipeline / "bridge_dispatch.log"
        changed = VALID.replace("in[1]=aa", "in[1]=cc").replace("out[1]=bb", "out[1]=dd")
        log.write_text(OTHER.replace("out[1]=00", "out[1]=ff") + "\n" + changed + "\n" + changed + "\n")
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"checked 2 functions; failures: none", result.stdout)
        with (self.pipeline / "dispatch_layouts.csv").open() as f:
            row = next(csv.DictReader(f))
        self.assertEqual((row["calls"], row["in_hex"], row["out_hex"]), ("2", "cc", "dd"))

    def test_pipeline_rejects_unexpected_call_and_reference_size_or_symbol_drift(self):
        for mutation in [dict(out_size="2", out_hex="bbcc"), dict(symbol="WrongSymbol")]:
            with self.subTest(mutation=mutation):
                self.change_csv("submit_layouts.csv", lambda rows: rows[0].update(mutation))
                expected = {name: (self.pipeline / name).read_bytes() for name in OUTPUTS}
                result = self.run_pipeline()
                self.assertNotEqual(result.returncode, 0, result.stdout.decode())
                self.assertIn(b"fill", result.stderr)
                self.assertIn(b"structure", result.stderr)
                for name, raw in expected.items():
                    self.assertEqual((self.pipeline / name).read_bytes(), raw, name)
                (self.pipeline / "submit_layouts.csv").write_bytes(self.baseline["submit_layouts.csv"])
        (self.pipeline / "bridge_fill.log").write_text(VALID + "\n" + OTHER + "\n")
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn(b"fill", result.stderr)
        for name, raw in self.baseline.items():
            self.assertEqual((self.pipeline / name).read_bytes(), raw, name)

    def test_saved_dispatch_kick_removal_and_connect_truncation_fail(self):
        # Real saved logs/reference CSVs; synthetic wrapper instructions use only the
        # existing IDs and sizes. No vendor binaries or new field decoding are needed.
        captures = ["bringup_layouts.csv", "submit_layouts.csv", "dispatch_layouts.csv"]
        wrappers = {}
        for name in captures:
            shutil.copy2(ROOT / name, self.pipeline / name)
            with (ROOT / name).open() as f:
                for row in csv.DictReader(f):
                    wrappers.setdefault(row["symbol"], row)
        for phase in ["bringup", "fill", "dispatch"]:
            shutil.copy2(ROOT / f"bridge_{phase}.log", self.pipeline / f"bridge_{phase}.log")
        symbols, instructions = [], []
        for index, (symbol, row) in enumerate(wrappers.items(), 1):
            base = index * 0x100
            symbols.append(f"{base:x} {symbol}")
            ops = [f"mov w1, #{row['bridge']}", f"mov w2, #{row['func']}", "mov x3, sp",
                   f"mov w4, #{row['in_size']}", "add x5, sp, #256", f"mov w6, #{row['out_size']}",
                   "bl 37a70 <PVRSRVBridgeCall>", "ret"]
            instructions.extend(f" {base + i * 4:x}:\t{op}" for i, op in enumerate(ops))
        (self.vendor / "syms.txt").write_text("\n".join(symbols) + f"\n{(len(wrappers) + 1) * 0x100:x} End\n")
        (self.vendor / "srv_um.dis").write_text("\n".join(instructions) + "\n")
        result = self.run_pipeline()
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn(b"checked 36 functions; failures: none", result.stdout)
        expected = {name: (self.pipeline / name).read_bytes() for name in OUTPUTS}
        log = self.pipeline / "bridge_dispatch.log"
        lines = log.read_text().splitlines()
        kicks = [line for line in lines if line.startswith("B bridge=129 func=8 ")]
        self.assertEqual(len(kicks), 1)
        connect = next(line for line in lines if line.startswith("B bridge=1 func=0 "))
        for label, modified in [("missing kick", [line for line in lines if line not in kicks]),
                                ("Connect-only", [connect])]:
            with self.subTest(case=label):
                log.write_text("\n".join(modified) + "\n")
                result = self.run_pipeline()
                self.assertNotEqual(result.returncode, 0, result.stdout.decode())
                self.assertIn(b"dispatch", result.stderr)
                self.assertIn(b"BridgeRGXKickCDM", result.stderr)
                self.assertNotIn(b"failures: none", result.stdout)
                for name, raw in expected.items():
                    self.assertEqual((self.pipeline / name).read_bytes(), raw, name)

    def test_unknown_cli_modes_fail_without_device_access(self):
        result = self.run_script("decode_layouts.py", "--typo")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"usage:", result.stderr)
        result = self.run_pipeline("--typo")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"usage:", result.stderr)
        for name, expected in self.baseline.items():
            self.assertEqual((self.pipeline / name).read_bytes(), expected, name)

    def test_missing_pipeline_phase_fails_without_publishing(self):
        (self.pipeline / "bridge_dispatch.log").unlink()
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"missing", result.stderr)
        for name, expected in self.baseline.items():
            self.assertEqual((self.pipeline / name).read_bytes(), expected, name)

    def test_pipeline_propagates_wrapper_failure_through_tee(self):
        path = self.vendor / "srv_um.dis"
        path.write_text(path.read_text().replace("mov w6, #1", "mov w6, #2", 1))
        result = self.run_pipeline()
        self.assertNotEqual(result.returncode, 0, result.stdout.decode())
        self.assertIn(b"out size", result.stderr)
        for name, expected in self.baseline.items():
            self.assertEqual((self.pipeline / name).read_bytes(), expected, name)


if __name__ == "__main__":
    unittest.main()
