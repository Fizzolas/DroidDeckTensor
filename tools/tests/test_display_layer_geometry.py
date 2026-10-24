"""The display layer's geometry must be stated once, in one frame of reference.

`ASurfaceTransaction_setGeometry(source, destination, transform)` (NDK `surface_control.h`):

  * `destination` - "the rect in the PARENT's space where this surface will be drawn"
  * `transform`   - "applied AFTER the source rect is applied to the buffer"

So `destination` is the FINAL rect. Passing both a destination already computed in the rotated
parent frame AND a non-zero transform applies the rotation twice. Measured on the Pixel 10
(dumpsys SurfaceFlinger):

    Layer droiddeck_wayland_game
      opaqueRegion={0,0,1650,732}  toDisplayTransform={ scale x=3.2842 y=0.6467 }
      bounds={0,6,1073,2404}

A ROT_90 of a 1650x732 source must land on 732x1650; 1073x2404 with a non-uniform 3.28/0.65 stretch
is a per-scanline misread - the horizontal random-colour-lines symptom.

The scene -> parent mapping already swaps the axes for a 90/270 surface
(`vkp_update_map` / `g_map_scene_swapped`), so the rect `vkp_map_rect` returns is already in the
parent's space and the transform must be 0.
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

    def test_the_map_already_swaps_the_scene_for_a_rotated_surface(self):
        """Basis for the fix: the destination handed to setGeometry is already rotated."""
        body = self.vk.split('int vkp_update_map(', 1)[1].split('\n}', 1)[0]
        self.assertIn('g_map_scene_swapped', body)
        self.assertIn('scene_w = scene_h', body)

    def test_setgeometry_does_not_rotate_a_destination_that_is_already_rotated(self):
        call = re.search(r'api\.setGeometry\([^;]*\);', self.sc)
        self.assertIsNotNone(call, 'setGeometry call not found')
        self.assertIn(', 0);', call.group(0),
                      'setGeometry must pass transform 0: the destination from vkp_map_rect is '
                      'already in the parent frame, and the contract applies transform after it')

    def test_the_transform_helper_survives_for_the_paths_that_need_it(self):
        """It is still used to learn the rotation; it just must not be passed to setGeometry."""
        self.assertIn('vkp_surface_transform_hal', self.vk)

    def test_the_planar_path_still_states_its_own_transform(self):
        """vkp_blit_scene_to feeds the copy path, whose destination is NOT pre-swapped."""
        self.assertIn('vkp_blit_scene_to', self.vk)


if __name__ == '__main__':
    unittest.main()
