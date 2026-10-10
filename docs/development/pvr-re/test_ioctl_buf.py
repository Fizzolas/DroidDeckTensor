"""Native offline hook regression: python -m unittest discover -s docs/development/pvr-re -v

Requires a host C compiler (HOST_CC, cc, gcc or clang). On Windows, a tiny
RTLD_NEXT header shim lets MinGW compile the source; dlsym itself is mocked by
the harness. Android/bionic compilation is checked separately with NDK clang.
"""
import csv
import errno
import importlib.util
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent


class HookBoundaryTests(unittest.TestCase):
    def test_actual_hook_at_controlled_ioctl_boundary(self):
        compiler = os.environ.get("HOST_CC") or shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(compiler, "native hook tests require HOST_CC or a host C compiler")
        with tempfile.TemporaryDirectory(prefix="pvr-hook-test-") as tmp:
            work = Path(tmp)
            flags = []
            if sys.platform == "win32":
                # Only RTLD_NEXT is needed: the harness replaces dynamic lookup, not the hook logic.
                (work / "dlfcn.h").write_text("#define RTLD_NEXT ((void *)-1)\n")
                flags = ["-I", str(work)]
            binary = work / ("test_ioctl_buf.exe" if sys.platform == "win32" else "test_ioctl_buf")
            built = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", *flags,
                                    str(ROOT / "hooks/test_ioctl_buf.c"), "-o", str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("PASS "), 12)
            self.assertIn("12 controlled ioctl cases passed", result.stdout)
            print(result.stdout, end="")
            diagnostics = []
            for operation in ["snapshot", "open", "write", "close", "readonly"]:
                for outcome in ["success", "failure", "stale"]:
                    for request in (["bridge"] if operation == "snapshot" else ["bridge", "other"]):
                        with self.subTest(operation=operation, outcome=outcome, request=request):
                            loss = subprocess.run([str(binary), operation, outcome, request], cwd=work,
                                                  capture_output=True, text=True, timeout=10)
                            # Native assertions check forwarding, return, errno and buffer mutation.
                            self.assertEqual(loss.returncode, 0, loss.stdout + loss.stderr)
                            self.assertIn("pvr-capture-loss:", loss.stderr)
                            self.assertIn("write" if operation == "readonly" else operation, loss.stderr)
                            self.assertRegex(loss.stderr, r"errno=[0-9]+")
                            diagnostics.append(loss.stderr)
            # A failed write, whether it returned an error or came up short, must be named by
            # the errno of that write: the harness fails one write with ENOSPC and then leaves
            # ERANGE behind for every write that follows.
            for mode, message in [("write-errno", "write errno names the failed write"),
                                  ("partial-write", "partial write errno names the failed write")]:
                with self.subTest(mode=mode):
                    failed_write = subprocess.run([str(binary), mode], cwd=work,
                                                  capture_output=True, text=True, timeout=10)
                    self.assertEqual(failed_write.returncode, 0,
                                     failed_write.stdout + failed_write.stderr)
                    self.assertIn(f"pvr-capture-loss: write errno={errno.ENOSPC}",
                                  failed_write.stderr)
                    self.assertNotIn(f"errno={errno.ERANGE}", failed_write.stderr)
                    self.assertIn(f"PASS {message}", failed_write.stdout)
            # Decode an actual record emitted by the compiled hook, not a handwritten fixture.
            spec = importlib.util.spec_from_file_location("hook_decoder", ROOT / "decode_layouts.py")
            decoder = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(decoder)
            shutil.copy2(ROOT / "bridge_table.csv", work / "bridge_table.csv")
            decoder.ROOT = work
            log = work / "bridge.log"
            out = work / "capture.csv"
            success = subprocess.run([str(binary), "valid"], capture_output=True, text=True, timeout=10)
            self.assertEqual(success.returncode, 0, success.stderr)
            self.assertEqual(success.stderr, "")
            log.write_text(success.stdout)
            decoder.from_log(log, out)
            with out.open() as f:
                row = next(csv.DictReader(f))
            self.assertEqual((row["in_hex"], row["out_hex"]), ("aabbcc", "778899"))
            failed = subprocess.run([str(binary), "valid", "failure"],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(failed.returncode, 0, failed.stderr)
            log.write_text(failed.stdout)
            prior = out.read_bytes()
            with self.assertRaisesRegex(ValueError, "bridge ioctl returned -1"):
                decoder.from_log(log, out)
            self.assertEqual(out.read_bytes(), prior)
            # A complete-looking record with a close failure still carries loss on stderr.
            closed = subprocess.run([str(binary), "close"], capture_output=True, text=True, timeout=10)
            self.assertEqual(closed.returncode, 0, closed.stderr)
            log.write_text(closed.stdout)
            decoder.from_log(log, out)
            marker_match = re.search(r'#define BRIDGE_CAPTURE_LOSS_MARKER "([^"]+)"',
                                     (ROOT / "hooks/bridge_abi.h").read_text())
            self.assertIsNotNone(marker_match)
            marker = marker_match[1]
            # Feed the real hook diagnostic into --capture via fake shell boundaries.
            # No real adb or NDK command is invoked by this test.
            pipeline = work / "pipeline"
            shutil.copytree(ROOT, pipeline, ignore=shutil.ignore_patterns("__pycache__"))
            outputs = ["bridge_table.csv", "bringup_layouts.csv", "submit_layouts.csv",
                       "dispatch_layouts.csv", "decode_out.txt", "bridge_field_map.md"]
            baseline = {name: (pipeline / name).read_bytes() for name in outputs}
            vendor = work / "vendor"
            vendor.mkdir()
            # Use only existing reference IDs/sizes for the no-loss capture control.
            wrappers = {}
            for capture in ["bringup_layouts.csv", "submit_layouts.csv", "dispatch_layouts.csv"]:
                with (ROOT / capture).open() as f:
                    for row in csv.DictReader(f):
                        wrappers.setdefault(row["symbol"], row)
            symbols, instructions = [], []
            for index, (symbol, row) in enumerate(wrappers.items(), 1):
                base = index * 0x100
                symbols.append(f"{base:x} {symbol}")
                ops = [f"mov w1, #{row['bridge']}", f"mov w2, #{row['func']}", "mov x3, sp",
                       f"mov w4, #{row['in_size']}", "add x5, sp, #256", f"mov w6, #{row['out_size']}",
                       "bl 37a70 <PVRSRVBridgeCall>", "ret"]
                instructions.extend(f" {base + i * 4:x}:\t{op}" for i, op in enumerate(ops))
            (vendor / "syms.txt").write_text("\n".join(symbols) + f"\n{(len(wrappers) + 1) * 0x100:x} End\n")
            (vendor / "srv_um.dis").write_text("\n".join(instructions) + "\n")
            (work / "diagnostic.txt").write_text(closed.stderr)
            driver = work / "fake-capture.sh"
            driver.write_text('''#!/usr/bin/env bash
set -o pipefail
# An injected checker failure for the fail-closed regression; inert unless GREP_BROKEN is set.
grep() {
    if [ -n "${GREP_BROKEN:-}" ]; then
        echo "grep: injected failure" >&2
        return 2
    fi
    command grep "$@"
}
adb() {
    printf '%s\\n' "$*" >> "$CALLS"
    if [ "$1" = shell ] && [[ "$2" == *LD_PRELOAD* ]]; then
        case "$2" in
            *./dispatch*) phase=dispatch ;;
            *./vksubmit\\ fill*) phase=fill ;;
            *) phase=bringup ;;
        esac
        printf '%s' "$phase" > "$PHASE_FILE"
        if [ "$phase" = "$LOSS_PHASE" ]; then cat "$DIAGNOSTIC" >&2; fi
        printf 'rc=0\\n'
    elif [ "$1" = pull ]; then
        if [ "$(cat "$PHASE_FILE")" = "$LOSS_PHASE" ] && command grep -Fq 'pvr-capture-loss:' "$DIAGNOSTIC"; then
            echo 'UNEXPECTED LOG PULL' >&2
            return 99
        fi
        target="$3"
        if command -v cygpath >/dev/null 2>&1; then target="$(cygpath -u "$target")"; fi
        cp "$PIPELINE/bridge_$(cat "$PHASE_FILE").log" "$target"
    fi
}
fake_clang() { return 0; }
export -f grep adb fake_clang
export NDK_CLANG=fake_clang
bash "$PIPELINE/run_pipeline.sh" --capture
''')
            env = os.environ.copy()
            env.update(PVR_VENDOR=vendor.as_posix(), PIPELINE=pipeline.as_posix(),
                       DIAGNOSTIC=(work / "diagnostic.txt").as_posix(), CALLS=(work / "calls.txt").as_posix(),
                       PHASE_FILE=(work / "phase.txt").as_posix(), PYTHON=Path(sys.executable).as_posix(),
                       TMPDIR=work.as_posix())
            env.pop("LOG_DIR", None)
            env.pop("ADB_SERIAL", None)
            env.pop("BASH_ENV", None)
            bash = "C:/Program Files/Git/usr/bin/bash.exe" if sys.platform == "win32" else shutil.which("bash")
            self.assertIsNotNone(bash)
            # This marker may occur early in very long output: do not inspect only the tail.
            cases = [("bringup", text) for text in diagnostics]
            cases += [("bringup", closed.stderr + ("probe chatter\n" * 2000)),
                      ("fill", closed.stderr), ("dispatch", closed.stderr)]
            for phase, transcript in cases:
                env["LOSS_PHASE"] = phase
                (work / "diagnostic.txt").write_text(transcript)
                rejected = subprocess.run([bash, driver.as_posix()], env=env,
                                          capture_output=True, text=True, timeout=30)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn(f"{phase} hook reported capture loss", rejected.stderr)
                self.assertNotIn("failures: none", rejected.stdout)
                self.assertIn(marker, rejected.stderr)
                self.assertNotIn("UNEXPECTED LOG PULL", rejected.stderr)
                for name, raw in baseline.items():
                    self.assertEqual((pipeline / name).read_bytes(), raw, name)
            (work / "diagnostic.txt").write_text("harmless probe warning\n")
            accepted = subprocess.run([bash, driver.as_posix()], env=env,
                                      capture_output=True, text=True, timeout=30)
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            self.assertIn("checked 36 functions; failures: none", accepted.stdout)
            for name, raw in baseline.items():
                self.assertEqual((pipeline / name).read_bytes(), raw, name)
            # A checker that cannot answer must fail the phase, not pass it: a run whose loss
            # check fails is refused whether or not the probe output carried a diagnostic.
            env["LOSS_PHASE"] = "dispatch"
            env["GREP_BROKEN"] = "1"
            (work / "diagnostic.txt").write_text("harmless probe warning\n")
            unchecked = subprocess.run([bash, driver.as_posix()], env=env,
                                       capture_output=True, text=True, timeout=30)
            self.assertNotEqual(unchecked.returncode, 0, unchecked.stdout)
            self.assertIn("cannot check the probe output for capture loss", unchecked.stderr)
            self.assertNotIn("failures: none", unchecked.stdout)
            (work / "diagnostic.txt").write_text(closed.stderr)
            reported = subprocess.run([bash, driver.as_posix()], env=env,
                                      capture_output=True, text=True, timeout=30)
            self.assertNotEqual(reported.returncode, 0, reported.stdout)
            self.assertIn("cannot check the probe output for capture loss", reported.stderr)
            self.assertNotIn("failures: none", reported.stdout)
            self.assertNotIn("UNEXPECTED LOG PULL", reported.stderr)
            for name, raw in baseline.items():
                self.assertEqual((pipeline / name).read_bytes(), raw, name)
            del env["GREP_BROKEN"]
            # The generic parser must never silently ignore a diagnostic accidentally merged into a log.
            log.write_text(success.stdout + closed.stderr)
            with self.assertRaises(ValueError):
                decoder.from_log(log, out)


if __name__ == "__main__":
    unittest.main()
