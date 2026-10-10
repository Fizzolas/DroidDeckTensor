package com.droiddeck.launcher.gpu

import com.droiddeck.launcher.gpu.GpuInfo.Family
import com.droiddeck.launcher.gpu.GpuInfo.Support
import com.droiddeck.launcher.gpu.GpuInfo.Vendor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/** A Mali GPU of [family]. Shared with the Robolectric wording tests. */
internal fun maliGpu(family: Family) = GpuInfo(family.label, 0, family, "Tensor G4", false)

/**
 * An Adreno of [model], with the family detection gives it and KGSL's own name for it. A model of 0
 * is the Adreno a kernel names without its model ("Adreno33v2"): no digits, and no name to read.
 */
internal fun adrenoGpu(model: Int, kgslName: String = if (model > 0) "Adreno$model" else "") =
    GpuInfo(if (model > 0) "Adreno $model" else "Adreno", model, GpuInfo.familyOf(true, model), "", false, kgslName)

/** A GPU of neither vendor: the app has nothing for it, and that is unchanged. */
internal fun otherGpu() = GpuInfo("Xclipse 940", 0, GpuInfo.familyOf(false, 0), "", false)

/**
 * Which GPU a device has, and what the app makes of it: the two vendors it recognises (Qualcomm's
 * Adreno through KGSL, Arm's Mali through kbase), the detection that tells them apart, and the Google
 * Tensor mapping that puts a Mali in the right generation. Each probe is a real path in a temp folder,
 * so none of it needs a phone.
 */
class GpuVendorTest {
    @get:Rule val files = TemporaryFolder()

    /** A fresh path, present as a directory, or as a file when [file]; or absent. */
    private fun node(present: Boolean, file: Boolean = false): File =
        File(files.newFolder(), "node").also { if (present) if (file) it.writeText("") else it.mkdirs() }

    @Test
    fun eachVendorIsRecognisedByWhatOnlyItsStackHas() {
        // Adreno: the KGSL node, or the vendor's own Vulkan HAL at its usual path.
        assertTrue(GpuInfo.isAdreno(kgslDir = node(true), vulkanDriver = node(false)))
        assertTrue(GpuInfo.isAdreno(kgslDir = node(false), vulkanDriver = node(true, file = true)))
        assertFalse(GpuInfo.isAdreno(kgslDir = node(false), vulkanDriver = node(false)))
        // Mali: kbase's node, the class a misc-device build registers itself in, or Arm's Vulkan HAL.
        assertTrue(GpuInfo.isMali(kbaseDevice = node(true, file = true), kbaseClass = node(false), vulkanDriver = node(false)))
        assertTrue(GpuInfo.isMali(kbaseDevice = node(false), kbaseClass = node(true), vulkanDriver = node(false)))
        assertTrue(GpuInfo.isMali(kbaseDevice = node(false), kbaseClass = node(false), vulkanDriver = node(true, file = true)))
        assertFalse(GpuInfo.isMali(kbaseDevice = node(false), kbaseClass = node(false), vulkanDriver = node(false)))
    }

    @Test
    fun aPhoneThatLooksLikeBothIsAnAdreno() {
        assertEquals(Vendor.ADRENO, GpuInfo.vendorOf(adreno = true, mali = true))
        assertEquals(Vendor.ADRENO, GpuInfo.vendorOf(adreno = true, mali = false))
        assertEquals(Vendor.MALI, GpuInfo.vendorOf(adreno = false, mali = true))
        assertEquals(Vendor.OTHER, GpuInfo.vendorOf(adreno = false, mali = false))
    }

    @Test
    fun everyFamilyBelongsToAVendor() {
        for (family in listOf(Family.A8XX, Family.A7XX_LOW, Family.A7XX, Family.A6XX, Family.ADRENO_UNKNOWN)) {
            assertEquals(family.name, Vendor.ADRENO, family.vendor)
        }
        for (family in listOf(Family.MALI_G715, Family.MALI_G78, Family.MALI_UNKNOWN)) {
            assertEquals(family.name, Vendor.MALI, family.vendor)
        }
        assertEquals(Vendor.OTHER, Family.NOT_ADRENO.vendor)
    }

    @Test
    fun aTensorsMaliIsReadFromTheGpuThePlatformOrTheModel() {
        // The GPU naming itself is the strongest signal: it is the name the driver lists use.
        assertEquals(Family.MALI_G715, GpuInfo.maliFamilyOf("Mali-G715", "", "", ""))
        assertEquals(Family.MALI_G78, GpuInfo.maliFamilyOf("Mali-G78 MP20", "", "", ""))
        // ... and it wins over a platform that would have placed the GPU otherwise.
        assertEquals(Family.MALI_G78, GpuInfo.maliFamilyOf("Mali-G78", "", "", "zumapro"))
        // Before Vulkan has been asked (it names the GPU), the model and platform code names stand in.
        assertEquals(Family.MALI_G78, GpuInfo.maliFamilyOf("", "gs101", "", ""))
        assertEquals(Family.MALI_G78, GpuInfo.maliFamilyOf("", "", "gs201", ""))
        assertEquals(Family.MALI_G715, GpuInfo.maliFamilyOf("", "", "", "zuma"))
        assertEquals(Family.MALI_G715, GpuInfo.maliFamilyOf("", "GS401", "", "zumapro"))
        // Case does not matter.
        assertEquals(Family.MALI_G715, GpuInfo.maliFamilyOf("MALI-G715", "", "", "ZUMA"))
        // A Mali the app cannot place stays unknown rather than being given a generation it may not
        // be: the digits of a Mali model do not run in generation order (a G78 predates a G715, but a
        // G310 is newer than both).
        assertEquals(Family.MALI_UNKNOWN, GpuInfo.maliFamilyOf("Mali-G57", "", "", "somewhere"))
        assertEquals(Family.MALI_UNKNOWN, GpuInfo.maliFamilyOf("", "", "", ""))
    }

    @Test
    fun aMaliIsRecognisedRatherThanRefused() {
        // A Mali is not "not supported", it is untested hardware; a GPU of neither vendor is still refused.
        for (family in listOf(Family.MALI_G715, Family.MALI_G78, Family.MALI_UNKNOWN)) {
            assertEquals(family.name, Support.UNTESTED, maliGpu(family).support)
        }
        assertEquals(Support.UNSUPPORTED, otherGpu().support)
    }
}
