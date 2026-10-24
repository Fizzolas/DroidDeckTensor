import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RIG = ROOT / 'docs/development/pvr-re/rig'
COMPOSITOR = ROOT / 'app/src/main/cpp/waylandcomp'


class CalibratorSourceTest(unittest.TestCase):
    def setUp(self):
        self.src = (RIG / 'calibrate.c').read_text(encoding='utf-8')

    def test_it_supports_the_four_modes(self):
        for mode in ('mmap', 'dlopen', 'crash', 'mark'):
            self.assertIn('"%s"' % mode, self.src)

    def test_the_crash_mode_uses_a_fixed_address_it_also_prints(self):
        self.assertIn('CAL-CRASH', self.src)
        self.assertIn('0xdeadbe0', self.src.lower())

    def test_the_build_script_builds_it(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('calibrate', build)

    def test_it_does_not_require_glibc_newer_than_the_runtime_floor(self):
        build = (RIG / 'build.sh').read_text(encoding='utf-8')
        self.assertIn('aarch64-linux-gnu.2.31', build)


class CompositorDigestTest(unittest.TestCase):
    def setUp(self):
        self.src = (COMPOSITOR / 'src/vk_present.c').read_text(encoding='utf-8')
        self.hdr = (COMPOSITOR / 'src/vk_present.h').read_text(encoding='utf-8')
        self.cmake = (COMPOSITOR / 'CMakeLists.txt').read_text(encoding='utf-8')

    def test_the_digest_is_compiled_in(self):
        self.assertIn('src/frame_digest.c', self.cmake)

    def test_the_staging_image_is_sized_from_the_source_extent(self):
        """The defect that produced §65-68 was a fixed-size staging buffer. This forbids one."""
        body = self.src.split('static int fd_ensure_staging', 1)[1].split('int vkp_frame_digest', 1)[0]
        self.assertIn('src->w', body)
        self.assertIn('src->h', body)
        self.assertIn('GetImageSubresourceLayout', body)
        self.assertNotIn('256', body)

    def test_the_staging_image_takes_the_sources_format(self):
        """A different format would swap channels and make the digest incomparable."""
        body = self.src.split('static int fd_ensure_staging', 1)[1].split('int vkp_frame_digest', 1)[0]
        self.assertIn('.format = src->fmt', body)

    def test_the_digest_reads_at_the_drivers_row_pitch(self):
        body = self.src.split('int vkp_frame_digest', 1)[1]
        self.assertIn('row_pitch', body)
        self.assertNotIn('src->w * 4, &d', body)

    def test_the_readback_does_not_reuse_the_cursor_helper(self):
        body = self.src.split('int vkp_frame_digest', 1)[1]
        self.assertNotIn('vkp_image_readback', body.split('the compositor pass', 1)[0])

    def test_sampling_is_adaptive_not_fixed(self):
        self.assertIn('fd_should_sample', self.src)
        self.assertIn('frame < 120', self.src)

    def test_the_digest_is_off_by_default(self):
        self.assertIn('BL_FRAME_DIGEST', self.src)
        self.assertRegex(self.src, r'getenv\("BL_FRAME_DIGEST"\)')

    def test_the_display_path_composes_before_it_digests(self):
        """The call must come after the compose wait, so it measures a finished frame."""
        body = self.src.split('static int render_display_impl', 1)[1]
        self.assertLess(body.index('timed_wait'), body.index('vkp_frame_digest'))

    def test_the_header_declares_the_digest(self):
        self.assertIn('int vkp_frame_digest', self.hdr)

    def test_the_switch_crosses_from_java_because_the_compositor_is_in_the_app_process(self):
        """A variable exported to the guest never reaches the compositor; it runs in our process."""
        jni = (COMPOSITOR / 'src/waylandcomp_jni.c').read_text(encoding='utf-8')
        java = (ROOT / 'app/src/main/java/com/droiddeck/launcher/wayland/WaylandCompositor.java').read_text(encoding='utf-8')
        activity = (ROOT / 'app/src/main/java/com/droiddeck/launcher/SessionActivity.kt').read_text(encoding='utf-8')
        self.assertIn('nativeSetFrameDigest', jni)
        self.assertIn('nativeSetFrameDigest', java)
        self.assertIn('nativeSetFrameDigest', activity)

    def test_the_jni_setter_clamps_the_level(self):
        jni = (COMPOSITOR / 'src/waylandcomp_jni.c').read_text(encoding='utf-8')
        body = jni.split('nativeSetFrameDigest', 1)[1].split('}', 1)[0]
        self.assertIn('level > 2', body)


if __name__ == '__main__':
    unittest.main()
