"""The display layer's geometry must be stated once, in one frame of reference.

`ASurfaceTransaction_setGeometry(source, destination, transform)` (NDK `surface_control.h`):

  * `destination` - "the rect in the PARENT's space where this surface will be drawn"
  * `transform`   - "applied AFTER the source rect is applied to the buffer"

The parent of ASurfaceControl is the window (ANativeWindow), which is already in
sensorLandscape orientation (e.g. 2404x1080). Destination rect dstR is mapped directly
in the window's coordinate space with uniform scaling and aspect ratio preserved.
SurfaceFlinger handles rotation of the parent window to the physical display panel.

Neither the parent extent nor the scene dimensions should be swapped. Swapping axes
causes severe 5x non-uniform stretch / scanline shearing.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SC_LAYER = ROOT / 'app/src/main/cpp/waylandcomp/src/sc_layer.c'
VK_PRESENT = ROOT / 'app/src/main/cpp/waylandcomp/src/vk_present.c'


class DisplayLayerGeometryTest(unittest.TestCase):
    def setUp(self):
        self.sc = SC_LAYER.read_text(encoding='utf-8')
        self.vk = VK_PRESENT.read_text(encoding='utf-8')

    def test_the_map_preserves_scene_aspect_without_swapping_axes(self):
        """The parent window is already landscape; axes must not be swapped."""
        body = self.vk.split('int vkp_update_map(', 1)[1].split('\n}', 1)[0]
        self.assertNotIn('scene_w = scene_h', body,
                         'vkp_update_map must not swap scene axes; parent window is already landscape')

    def test_setgeometry_does_not_rotate_a_destination_that_is_already_rotated(self):
        call = re.search(r'api\.setGeometry\([^;]*\);', self.sc)
        self.assertIsNotNone(call, 'setGeometry call not found')
        self.assertIn(', 0);', call.group(0),
                      'setGeometry must pass transform 0: the destination from vkp_map_rect is '
                      'already in the parent window frame, and the contract applies transform after it')

    def test_the_transform_helper_survives_for_the_paths_that_need_it(self):
        """It is still used to learn the rotation; it just must not be passed to setGeometry."""
        self.assertIn('vkp_surface_transform_hal', self.vk)

    def test_the_planar_path_still_states_its_own_transform(self):
        """vkp_blit_scene_to feeds the copy path, whose destination is NOT pre-swapped."""
        self.assertIn('vkp_blit_scene_to', self.vk)


if __name__ == '__main__':
    unittest.main()
