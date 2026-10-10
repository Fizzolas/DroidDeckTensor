package com.droiddeck.launcher.gpu

import android.content.Context
import com.droiddeck.launcher.R
import com.droiddeck.launcher.gpu.GpuInfo.Family
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/**
 * The GPU's wording: the card in the app's language (a resource per state), and the device report in
 * English whatever that language is. These cases need Android's resources, so they run under Robolectric
 * and are checked against the resource the code names, not a copy of its text.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class GpuSupportTextTest {
    private val context: Context = RuntimeEnvironment.getApplication()

    @Test
    fun aMaliIsExplainedAsExperimentalHardwareNotRefused() {
        for (family in listOf(Family.MALI_G715, Family.MALI_G78, Family.MALI_UNKNOWN)) {
            assertEquals(family.name, context.getString(R.string.gpuinfo_mali_experimental), maliGpu(family).supportText(context))
        }
        // The Snapdragon-only sentence is for a GPU of neither vendor.
        assertEquals(context.getString(R.string.gpuinfo_unsupported), otherGpu().supportText(context))
    }

    @Test
    fun theDeviceReportSaysItInEnglishWhateverTheAppLanguage() {
        // A Mali's line is the resource's own English; it must not carry the Snapdragon-only wording.
        assertEquals("Experimental: Mali support is new and untested", maliGpu(Family.MALI_G715).supportTextInEnglish(context))
        assertFalse(maliGpu(Family.MALI_G78).supportTextInEnglish(context).contains("Adreno"))
        // The Adreno lines keep the wording the reports have always carried.
        assertEquals("Supported", adrenoGpu(650).supportTextInEnglish(context))
        assertEquals("Outside tested hardware (Adreno 650, 725 and newer): it may not run", adrenoGpu(660).supportTextInEnglish(context))
        assertEquals("Experimental: its drivers are test builds", adrenoGpu(720).supportTextInEnglish(context))
        assertEquals("Not supported: DroidDeck needs an Adreno (Snapdragon) GPU", otherGpu().supportTextInEnglish(context))
    }

    @Test
    fun theDeviceReportStaysEnglishInAnotherAppLanguage() {
        // The card follows the app's language and the report does not: checked in Japanese, not the default.
        RuntimeEnvironment.setQualifiers("ja")
        try {
            assertEquals("サポート対象", context.getString(R.string.gpuinfo_supported))
            assertEquals("Supported", adrenoGpu(650).supportTextInEnglish(context))
            assertEquals("Experimental: Mali support is new and untested", maliGpu(Family.MALI_G715).supportTextInEnglish(context))
        } finally {
            RuntimeEnvironment.setQualifiers("")
        }
    }

    @Test
    fun aMaliNamesItselfRatherThanBeingATranslatedWord() {
        // A family's label is a driver list's key ("Mali-G715"), so it stays the same in every
        // language - unlike NOT_ADRENO, which is a sentence and is translated.
        assertEquals("Mali-G715", Family.MALI_G715.label(context))
        assertEquals("Mali-G78", Family.MALI_G78.label(context))
        assertEquals("Mali", Family.MALI_UNKNOWN.label(context))
        assertEquals(context.getString(R.string.gpuinfo_not_adreno), Family.NOT_ADRENO.label(context))
    }

    @Test
    fun theNonAdrenoCopyStatesTheOneFactItCanConfirm() {
        // Confirmed from code: a non-KGSL GPU gets no /dev/dri render node in the guest (LinuxRuntime
        // bindGpuNode returns when /dev/kgsl-3d0 is absent), and BwrapSpawner.gpu() binds nothing. So the
        // copy says the guest has no GPU device to draw on, and not that Steam shows a black screen.
        val tensor = "Google Tensor G1"
        val dialog = context.getString(R.string.main_non_adreno_text, tensor, 12.0)
        val setup = context.getString(R.string.setup_gpu_unsupported_detail, tensor)
        assertTrue(dialog, dialog.contains("Steam here has no GPU device to draw on"))
        assertTrue(setup, setup.contains("Steam has no GPU device to draw on"))
        assertFalse(dialog, dialog.contains("black screen"))
        assertFalse(setup, setup.contains("black screen"))
    }

    @Test
    fun theAdrenoWordingIsUntouched() {
        assertEquals(context.getString(R.string.gpuinfo_supported), adrenoGpu(650).supportText(context))
        assertEquals(context.getString(R.string.gpuinfo_below_tested), adrenoGpu(660).supportText(context))
        assertEquals(context.getString(R.string.gpuinfo_experimental), adrenoGpu(720).supportText(context))
    }

    @Test
    fun theNoDriverSentenceKeepsTheGpusWordingAsWritten() {
        // Auto's "nothing to set" line is built from the card's wording, not lowercased: Mali stays Mali.
        val sentence = context.getString(R.string.drivers_none_to_set, maliGpu(Family.MALI_G715).supportText(context))
        assertEquals("No drivers to set. " + context.getString(R.string.gpuinfo_mali_experimental), sentence)
        assertTrue(sentence.contains("Mali"))
    }

    @Test
    fun aPinnedDriverThatCannotRunHereSaysWhy() {
        val reason = context.getString(R.string.drivers_pinned_not_adreno, "Turnip 25.1.0")
        assertTrue(reason.contains("Turnip 25.1.0"))
        assertTrue(reason.contains("system Vulkan driver"))
    }
}
