import importlib.util
import json
import shutil
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f'diagkit/{name}.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


analyze = load('analyze')
manifest = load('manifest')


class AnalyzeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_a_finding_carries_artifact_and_line(self):
        f = analyze.finding('the layer path composed a frame', 'wayland.log', 42, 'clean')
        self.assertEqual(('wayland.log', 42), (f['artifact'], f['line']))
        self.assertEqual('', f['stamp'])

    def test_a_finding_on_an_untrusted_instrument_is_marked(self):
        f = analyze.finding('zero rows present', 'frame.log', 7, 'untrusted')
        self.assertEqual('UNTRUSTED', f['stamp'])

    def test_a_finding_on_a_tainted_run_is_marked(self):
        f = analyze.finding('x', 'a.log', 1, 'tainted')
        self.assertEqual('TAINTED', f['stamp'])

    def test_the_timeline_merges_sources_in_time_order(self):
        merged = analyze.merge_timeline({'a.log': [(1.0, 'first')], 'b.log': [(0.5, 'zeroth')]})
        self.assertEqual(['zeroth', 'first'], [t[2] for t in merged])

    def test_rendering_shows_the_citation_for_every_row(self):
        text = analyze.render([analyze.finding('claim', 'a.log', 3, 'clean')])
        self.assertIn('a.log:3', text)

    def test_a_run_with_no_digest_reports_it_rather_than_guessing(self):
        run = self.tmp / 'run'
        run.mkdir()
        (run / 'manifest.json').write_text(json.dumps(
            manifest.new_manifest('l', 'h', {'v': '1'}, 'c', 41)))
        found = analyze.findings_from(run)
        self.assertTrue(any('no digest' in f['claim'] for f in found))

    def test_a_digest_with_zero_rows_is_cited_at_its_own_line(self):
        run = self.tmp / 'run'
        run.mkdir()
        (run / 'manifest.json').write_text(json.dumps(
            manifest.new_manifest('l', 'h', {'v': '1'}, 'c', 41)))
        (run / 'wayland.log').write_text(
            'header\n00:00:01.000  fd   t=1.000 2404x1080 pitch=16384 zero_rows=0 hash=aa\n'
            '00:00:02.000  fd   t=2.000 2404x1080 pitch=16384 zero_rows=492 hash=bb\n')
        found = analyze.findings_from(run)
        row = next(f for f in found if '492' in f['claim'])
        self.assertEqual(('wayland.log', 3), (row['artifact'], row['line']))

    def test_a_calibrated_instrument_keeps_the_finding_clean(self):
        run = self.tmp / 'run'
        run.mkdir()
        m = manifest.new_manifest('l', 'h', {'v': '1'}, 'c', 41)
        manifest.stamp_instrument(m, 'frame-digest', 'sha256:aa', True, 'pattern recovered')
        (run / 'manifest.json').write_text(json.dumps(m))
        (run / 'wayland.log').write_text('fd t=1 8x8 pitch=32 zero_rows=0 hash=aa\n')
        self.assertEqual('clean', analyze.findings_from(run)[0]['trust'])

    def test_an_uncalibrated_instrument_downgrades_the_finding(self):
        run = self.tmp / 'run'
        run.mkdir()
        m = manifest.new_manifest('l', 'h', {'v': '1'}, 'c', 41)
        manifest.stamp_instrument(m, 'frame-digest', 'sha256:aa', False, 'pattern not recovered')
        (run / 'manifest.json').write_text(json.dumps(m))
        (run / 'wayland.log').write_text('fd t=1 8x8 pitch=32 zero_rows=0 hash=aa\n')
        self.assertEqual('untrusted', analyze.findings_from(run)[0]['trust'])


if __name__ == '__main__':
    unittest.main()
