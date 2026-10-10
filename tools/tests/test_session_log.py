import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

SESSION = Path(__file__).resolve().parents[1] / "linuxfs/overlay/usr/local/bin/droiddeck-session"


def log_redirect():
    match = re.search(r"^\s*(exec > >\(.*\) 2>&1)$", SESSION.read_text(), re.M)
    return match.group(1)


class SessionLogTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.log = self.tmp / "session.log"

    def run_session(self, body, cap=None):
        script = "BL_LOG=%s\n%s%s\n%s\n" % (self.log, "BL_LOG_CAP=%d\n" % cap if cap else "", log_redirect(), body)
        subprocess.run(["bash", "-c", script], timeout=30, check=False)

    def test_the_last_lines_survive_the_writer_being_killed(self):
        self.run_session('for i in $(seq 1 60); do echo "line $i"; done\nsleep 0.5\n'
                         'for p in $(pgrep -P $$); do kill -9 $(pgrep -P $p) $p 2>/dev/null; done')
        lines = self.log.read_text().splitlines()
        self.assertEqual(60, len(lines))
        self.assertEqual("line 60", lines[-1])

    def test_the_log_keeps_its_first_bytes_and_drains_the_rest(self):
        self.run_session("printf 'abcdefghij\\nklmnop\\n'\nsleep 0.5", cap=5)
        self.assertEqual("abcde", self.log.read_text())


if __name__ == "__main__":
    unittest.main()


class GamescopeSdlDriverTest(unittest.TestCase):
    """gamescope's own window is an SDL one, and it must be asked for SDL's Wayland driver.

    Left to auto-select in the guest, SDL handed Vulkan an EMPTY extension list, so gamescope built
    its instance without VK_KHR_wayland_surface, its backend never came up ("SDL_Vulkan_CreateSurface
    failed: VK_KHR_wayland_surface extension is not enabled in the Vulkan instance") and the session
    died before the client started. The outer pass execs gamescope; the inner pass (below) still
    points the client's toolkits at Xwayland, so this must not be a single global export.
    """

    def test_the_gamescope_exec_asks_for_the_wayland_driver(self):
        text = SESSION.read_text()
        match = re.search(r"^  export BL_INSIDE=1(.*)$\n  exec gamescope ", text, re.M)
        self.assertIsNotNone(match, "the gamescope exec should still export BL_INSIDE=1")
        self.assertIn("SDL_VIDEODRIVER=wayland", match.group(1),
                      "gamescope must be started with SDL's Wayland driver")
        self.assertIn("SDL_VIDEO_DRIVER=wayland", match.group(1),
                      "SDL3 reads the SDL_VIDEO_DRIVER hint name too")

    def test_the_inner_pass_still_sends_toolkits_to_xwayland(self):
        """gamescope manages X windows, so the client's toolkits go through Xwayland, not its socket."""
        text = SESSION.read_text()
        self.assertIn("export GDK_BACKEND=x11 QT_QPA_PLATFORM=xcb SDL_VIDEODRIVER=x11", text)
