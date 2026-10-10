"""Guards on the PowerVR shim ICD's source.

The ICD is C compiled by hand (see docs/development/pvr-re/hybshim/README.md), so nothing above
catches a defect in it at build time. These checks cover the two seams that each cost a device
session, and both are cheap enough to be source assertions:

* the instance create-info the HAL sees must have this ICD's own window-system extension names
  stripped from it, on EVERY path a loader can take. A loader that implements ICD interface 1 calls
  vk_icdCreateInstance directly; when that entrypoint forwarded the app's list untouched the
  PowerVR blob was handed VK_KHR_wayland_surface, which it does not know, and the session died with
  "VK_KHR_wayland_surface extension is not enabled in the Vulkan instance".
* the WSI must complete the semaphore/fence pair a real renderer's acquire hands it. Returning an
  image without signaling them leaves the app's first frame pending forever.
"""
import re
from pathlib import Path
import unittest


ICD = Path(__file__).resolve().parents[2] / 'docs/development/pvr-re/hybshim/icd'
PVR_ICD_C = ICD / 'pvr_icd.c'
PVR_WSI_C = ICD / 'pvr_wsi.c'


def source(path):
    return path.read_text(encoding='utf-8')


class InstanceExtensionStripTest(unittest.TestCase):
    """Every create-instance path must strip this ICD's extension names before the HAL sees them."""

    def test_the_icd_advertises_the_three_window_system_extensions(self):
        text = source(PVR_ICD_C)
        for name in ('VK_KHR_surface', 'VK_KHR_wayland_surface', 'VK_KHR_xcb_surface', 'VK_KHR_xlib_surface'):
            self.assertIn('"%s"' % name, text, name)

    def test_create_instance_strips_our_names(self):
        text = source(PVR_ICD_C)
        self.assertIn('icd_is_our_extension', text)
        # The strip must actually filter, not just be defined.
        body = re.search(r'static VkResult icd_create_instance\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'icd_create_instance not found')
        self.assertIn('icd_is_our_extension(names[i])', body.group(0))

    def test_the_direct_loader_entrypoint_routes_through_the_stripping_path(self):
        """A loader implementing ICD interface 1 calls vk_icdCreateInstance, not GetInstanceProcAddr.

        It must therefore do the same stripping work; forwarding the create-info straight to the HAL
        is what let VK_KHR_wayland_surface reach the blob and killed the session.
        """
        text = source(PVR_ICD_C)
        body = re.search(r'int vk_icdCreateInstance\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'vk_icdCreateInstance not found')
        entry = body.group(0)
        self.assertIn('icd_create_instance', entry,
                      'vk_icdCreateInstance must route through icd_create_instance to strip our extensions')
        self.assertNotIn('g_gipa(NULL, "vkCreateInstance")', entry,
                         'vk_icdCreateInstance must not call the HAL directly, or our WSI extensions reach it')
        self.assertNotIn('g_create', entry, 'vk_icdCreateInstance must not fall back to the HAL create')


class DrmPropertiesAreFilledInTest(unittest.TestCase):
    """The HAL advertises VK_EXT_physical_device_drm and then writes nothing into the struct.

    An app that zero-initialises it therefore reads hasPrimary=0 - and gamescope 3.16.29 gates its
    backend on exactly that (`if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary )`),
    so the nested Wayland backend was required to have a KMS primary node it never opens. The ICD
    must answer with the DRM device behind the node the HAL drives.
    """

    def test_the_icd_implements_get_physical_device_properties2(self):
        text = source(PVR_ICD_C)
        self.assertIn('icd_get_phys_dev_props2', text)

    def test_the_entrypoint_is_reachable_through_the_instance_proc_addr(self):
        """The loader's own trampoline calls the pointer returned from vk_icdGetInstanceProcAddr.

        Putting it in vk_icdGetPhysicalDeviceProcAddr instead is dead code: the loader only consults
        that for entry points it has no trampoline for, and this one has one.
        """
        text = source(PVR_ICD_C)
        body = re.search(r'void \*vk_icdGetInstanceProcAddr\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'vk_icdGetInstanceProcAddr not found')
        entry = body.group(0)
        self.assertIn('"vkGetPhysicalDeviceProperties2"', entry,
                      'the loader resolves this through vk_icdGetInstanceProcAddr')
        self.assertIn('icd_get_phys_dev_props2', entry)

    def test_the_drm_properties_struct_is_recognised_by_its_stype(self):
        text = source(PVR_ICD_C)
        self.assertIn('1000353000', text, 'VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT')

    def test_the_primary_node_is_found_through_sysfs_not_by_statting_it(self):
        """The guest's /dev/dri holds only the render node, so a card node has no path to stat.

        A phone app may not stat the real /dev/dri either - those nodes are graphics_device, which
        its policy does not grant - so the card node is discoverable only through the sysfs tree the
        app builds. Reading it by /dev/dri/card* would always fail and hasPrimary would stay false.
        """
        text = source(PVR_ICD_C)
        body = re.search(r'static int icd_find_primary_for\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'icd_find_primary_for not found')
        entry = body.group(0)
        self.assertIn('/device/drm', entry, 'the card node is found in the render node\'s device dir')
        self.assertIn('opendir', entry)
        self.assertIn('/dev', entry, 'the dev file is read from the sysfs entry')
        self.assertNotIn('stat(cpath', entry, 'the card node must not be stat-ed by its own path')

    def test_the_hal_still_answers_for_the_rest_of_the_chain(self):
        """The HAL must fill the other pNext structs; this ICD only corrects the DRM one."""
        text = source(PVR_ICD_C)
        body = re.search(r'static void icd_get_phys_dev_props2\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'icd_get_phys_dev_props2 not found')
        entry = body.group(0)
        self.assertIn('hal(pd, props2)', entry, 'the HAL still answers the call')
        self.assertIn('STYPE_DRM_PROPERTIES_EXT', entry)


class AcquireCompletesSemaphoreAndFenceTest(unittest.TestCase):
    """A real renderer acquires with a semaphore and/or a fence and waits on them."""

    def test_acquire_does_not_discard_the_semaphore_and_fence(self):
        text = source(PVR_WSI_C)
        body = re.search(r'static VkResult wsi_acquire_next_image\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'wsi_acquire_next_image not found')
        entry = body.group(0)
        self.assertNotIn('(void)sem', entry, 'the acquire semaphore must not be discarded')
        self.assertNotIn('(void)fence', entry, 'the acquire fence must not be discarded')
        self.assertIn('wsi_signal_on_queue', entry,
                      'the acquire must complete the semaphore/fence it was handed')

    def test_present_waits_on_the_render_semaphores(self):
        text = source(PVR_WSI_C)
        body = re.search(r'static VkResult wsi_queue_present\(.*?\n\}\n', text, re.S)
        self.assertIsNotNone(body, 'wsi_queue_present not found')
        entry = body.group(0)
        self.assertIn('pWaitSemaphores', entry,
                      'present must honor the semaphores the rendering was made to wait on')
        self.assertIn('wsi_wait_binary', entry)

    def test_the_internal_submits_come_from_the_hal_not_our_own_entrypoint(self):
        """Resolving vkQueueSubmit through our own dispatch would recurse into this file's entrypoint."""
        text = source(PVR_WSI_C)
        self.assertIn('pvr_wsi_register_device', text)
        self.assertIn('GetDeviceQueue', text, 'the app queue is needed before its first acquire')


class SurfaceExtentTest(unittest.TestCase):
    """currentExtent tells the app the exact size it must use, so it cannot be a guess."""

    def test_current_extent_is_not_a_hardcoded_size(self):
        text = source(PVR_WSI_C)
        body = re.search(r'static VkResult wsi_surface_caps\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'wsi_surface_caps not found')
        entry = body.group(0)
        self.assertNotIn('= 1280', entry, 'currentExtent must not be a hardcoded 1280')
        self.assertNotIn('= 720', entry, 'currentExtent must not be a hardcoded 720')
        self.assertIn('UINT32_MAX', entry,
                      'an unknown size must be reported as UINT32_MAX, the spec\'s "app chooses"')

    def test_the_x11_surface_reads_the_window_geometry_and_depth(self):
        text = source(PVR_WSI_C)
        body = re.search(r'static VkResult wsi_create_xcb_surface\(.*?\n\}', text, re.S)
        self.assertIsNotNone(body, 'wsi_create_xcb_surface not found')
        entry = body.group(0)
        self.assertIn('xcb_get_geometry', entry, 'the window size must be read, not assumed')
        # A DRI3 pixmap whose depth differs from its window is a protocol error.
        self.assertIn('g->depth', entry, 'the window depth must come from X, not a constant')


if __name__ == '__main__':
    unittest.main()
