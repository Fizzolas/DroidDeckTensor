"""The session's proot must let the guest create hard links.

Android refuses `link()` for apps (EACCES, measured at the exact path the Steam runtime extracts
into). Steam's runtime archive extracts with hard links as its dedup mechanism, so without a shim
the extraction fails partway - `Failed to unzip item 3732 (.../share/i18n/locales/anp_IN_)` - and
the client loops on "Installing update..." forever, which is the wall in state doc §64/§78.

proot's own answer is `--link2symlink`, which turns a guest `link()` into a symlink plus an entry
its loader resolves. `GuestCommand` has passed it all along; the session did not. These are source
assertions because the seam is a command-line the app builds, and the failure only appears on a
device.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / 'app/src/main/java/com/droiddeck/launcher'


class SessionProotLinkTest(unittest.TestCase):
    def setUp(self):
        self.runtime = (JAVA / 'runtime/LinuxRuntime.java').read_text(encoding='utf-8')
        self.service = (JAVA / 'session/SessionService.kt').read_text(encoding='utf-8')
        self.guest_command = (JAVA / 'runtime/GuestCommand.kt').read_text(encoding='utf-8')

    def test_the_session_proot_asks_for_the_hard_link_shim(self):
        self.assertIn('--link2symlink', self.runtime)

    def test_the_flag_is_in_the_shared_prefix_not_one_call_site(self):
        """Every proot the app builds must carry it, or the next caller reintroduces the bug."""
        body = self.runtime.split('public static List<String> prootPrefix(Context context, File root, String cwd, boolean fakeRoot)')[1]
        body = body.split('return cmd;', 1)[0]
        self.assertIn('--link2symlink', body)

    def test_the_one_off_command_still_has_its_own_shim(self):
        self.assertIn('--link2symlink', self.guest_command)

    def test_the_session_names_the_directory_the_shim_keeps_its_entries_in(self):
        """proot writes each hard link's stand-in there; unnamed it falls back to /tmp in the guest."""
        self.assertIn('PROOT_L2S_DIR', self.service)

    def test_the_directory_is_inside_the_app_and_not_the_guest_root(self):
        """Left unnamed, proot keeps the stand-ins in the guest rootfs, where they fill it up."""
        self.assertRegex(self.service, r'val linkDir = File\(filesDir, "\.l2s"\)')
        self.assertIn('hostEnv["PROOT_L2S_DIR"] = linkDir.path', self.service)


if __name__ == '__main__':
    unittest.main()
