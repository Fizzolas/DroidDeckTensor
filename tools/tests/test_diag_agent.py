import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
AGENT = ROOT / 'app/src/main/java/com/droiddeck/launcher/agent'


class AgentDiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.src = (AGENT / 'AgentDiagnostics.kt').read_text(encoding='utf-8')
        self.provider = (AGENT / 'AgentBridgeProvider.kt').read_text(encoding='utf-8')

    def test_the_provider_exposes_snapshot_and_mark(self):
        self.assertIn('"snapshot"', self.provider)
        self.assertIn('"mark"', self.provider)

    def test_snapshot_gathers_the_graphics_processes_and_the_app_identity(self):
        self.assertIn('/proc', self.src)
        self.assertIn('procs.txt', self.src)
        self.assertIn('app.txt', self.src)

    def test_the_app_does_not_attempt_a_dumpsys_it_is_not_allowed_to_read(self):
        """Measured on device: every dumpsys from the app is a Permission Denial. The host does it."""
        self.assertNotIn('dumpsys SurfaceFlinger', self.src)
        self.assertNotIn('dumpsys display', self.src)

    def test_snapshot_is_text_only(self):
        """The project's rule: no image is taken, not even for diagnostics."""
        self.assertNotIn('screencap', self.src)
        self.assertNotIn('Bitmap', self.src)

    def test_mark_writes_to_the_session_event_stream(self):
        self.assertIn('SessionEvents', self.src)
        self.assertIn('diag.mark', self.src)

    def test_the_snapshot_records_which_libraries_each_process_mapped(self):
        """Installed is not loaded; this is the field that says which binary actually ran."""
        self.assertIn('maps', self.src)
        self.assertIn('libvulkan_powervr', self.src)

    def test_the_provider_documents_the_new_methods(self):
        doc = (ROOT / 'docs/agent-control.md').read_text(encoding='utf-8')
        self.assertIn('--method snapshot', doc)
        self.assertIn('--method mark', doc)


if __name__ == '__main__':
    unittest.main()
