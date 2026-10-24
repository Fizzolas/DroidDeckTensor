import importlib.util
import shutil
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prior', TOOLS / 'diagkit/prior.py')
prior = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prior)


class PriorTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        (self.tmp / 'a.log').write_text('one\ncompositor renders on PowerVR\nthree\n')
        (self.tmp / 'b.txt').write_text('nothing relevant\n')

    def test_a_hit_names_the_file_and_the_line(self):
        hits = prior.search([self.tmp], 'PowerVR')
        self.assertEqual(1, len(hits))
        self.assertEqual(('a.log', 2), (hits[0]['artifact'], hits[0]['line']))

    def test_search_is_case_insensitive(self):
        self.assertTrue(prior.search([self.tmp], 'powervr'))

    def test_no_hit_is_an_empty_list_not_an_error(self):
        self.assertEqual([], prior.search([self.tmp], 'zzz-not-here'))

    def test_multiple_words_rank_by_how_many_are_present(self):
        (self.tmp / 'c.log').write_text('PowerVR\n')
        hits = prior.search([self.tmp], 'PowerVR compositor')
        self.assertEqual('a.log', hits[0]['artifact'])
        self.assertGreater(hits[0]['score'], hits[1]['score'])

    def test_binary_files_are_skipped(self):
        (self.tmp / 'blob.bin').write_bytes(b'\x00\x01PowerVR\x00')
        self.assertEqual([], [h for h in prior.search([self.tmp], 'PowerVR')
                              if h['artifact'] == 'blob.bin'])

    def test_a_missing_root_is_skipped_not_fatal(self):
        self.assertEqual([], prior.search([self.tmp / 'nope'], 'PowerVR'))

    def test_build_directories_are_skipped(self):
        (self.tmp / 'build').mkdir()
        (self.tmp / 'build' / 'generated.log').write_text('PowerVR\n')
        self.assertEqual([], [h for h in prior.search([self.tmp], 'PowerVR')
                              if 'generated' in h['artifact']])

    def test_an_empty_question_is_an_empty_result(self):
        self.assertEqual([], prior.search([self.tmp], '   '))


if __name__ == '__main__':
    unittest.main()
