import contextlib
import importlib.util
import io
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


if __name__ == '__main__':
    unittest.main()
