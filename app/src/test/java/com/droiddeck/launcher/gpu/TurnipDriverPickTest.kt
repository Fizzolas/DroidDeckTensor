package com.droiddeck.launcher.gpu

import com.droiddeck.launcher.gpu.GpuInfo.Family
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Which bundled display driver a GPU is handed. There are two stacks in the list - the Adreno
 * Turnips, which AdrenoTools loads by hooking KGSL, and Arm's PanVK, which drives the Mali on a
 * Google Tensor's kbase - and a GPU must never be handed the other vendor's build, whether the pick
 * chose it, the user pinned it, or the Downloads override asked for it. The decision is one pure
 * function in [TurnipDriver], so it is tested here without a device; whether the apk actually carries
 * a build's archive is a separate question, asked by the instance (`carried`).
 */
class TurnipDriverPickTest {
    @Test
    fun aMaliGetsTheMaliBuildAndOnlyForItsOwnGeneration() {
        // A Google Tensor G4 (Pixel 9): kbase, no KGSL. PanVK-on-kbase is the driver for it, in the
        // id the archive is named after (adrenotools-panvk-g715.tzst).
        assertEquals(TurnipDriver.DRIVER_MALI_G715, TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G715), null, 36))
        assertEquals(TurnipDriver.DRIVER_MALI_G715, TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G715), null, 30))
        // A Mali the app cannot place gets nothing rather than a build for a generation it may not
        // be: PanVK's binaries are per generation.
        assertNull(TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G78), null, 36))
        assertNull(TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_UNKNOWN), null, 36))
    }

    @Test
    fun aGpuOfNeitherVendorStillGetsNothing() {
        // An Xclipse or a PowerVR: unchanged. The app has no build for either stack.
        assertNull(TurnipDriver.bundledDriverFor(otherGpu(), null, 36))
        assertNull(TurnipDriver.bundledDriverFor(otherGpu(), null, 30))
    }

    @Test
    fun theOtherVendorsBuildIsRefusedHoweverItIsAskedFor() {
        // An Adreno Turnip named on a Mali - a stale pin, a restored backup, the Downloads
        // override - is refused exactly as a Mali build named on an Adreno is.
        assertNull(TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G715), "turnip25.1.0", 36))
        assertNull(TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G715), "turnip-sdk36", 36))
        assertNull(TurnipDriver.bundledDriverFor(adrenoGpu(750), TurnipDriver.DRIVER_MALI_G715, 36))
        assertNull(TurnipDriver.bundledDriverFor(adrenoGpu(830), TurnipDriver.DRIVER_MALI_G715, 33))
        // The Mali build asked for on the Mali it was built for is what it gets.
        assertEquals(TurnipDriver.DRIVER_MALI_G715, TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G715), TurnipDriver.DRIVER_MALI_G715, 36))
        // ... and on a Mali the build was not built for, it is refused too.
        assertNull(TurnipDriver.bundledDriverFor(maliGpu(Family.MALI_G78), TurnipDriver.DRIVER_MALI_G715, 36))
    }

    @Test
    fun aPinnedBuildIsRefusedExactlyWhereItIsNotUsed() {
        // What the menu shows a reason for, and what choose() refuses: the pin names the other
        // vendor's build. Every Adreno case is what it always was.
        assertTrue(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), "turnip25.1.0"))
        assertTrue(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), "turnip-sdk36"))
        assertTrue(TurnipDriver.refusesPinned(adrenoGpu(830), TurnipDriver.DRIVER_MALI_G715))
        assertTrue(TurnipDriver.refusesPinned(otherGpu(), "turnip25.1.0"))
        assertFalse(TurnipDriver.refusesPinned(adrenoGpu(830), "turnip-sdk36"))
        assertFalse(TurnipDriver.refusesPinned(adrenoGpu(750), "turnip25.1.0"))
        assertFalse(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), TurnipDriver.DRIVER_MALI_G715))
        // A pin that is not a bundled build at all is not this function's business: an imported
        // driver, a missing one, nothing.
        assertFalse(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), "imported-driver"))
        assertFalse(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), null))
        assertFalse(TurnipDriver.refusesPinned(maliGpu(Family.MALI_G715), ""))
    }

    @Test
    fun anAdrenoStillGetsTheBuildForItsGeneration() {
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(750), null, 34))
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(740, "adreno_740"), null, 34))
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(830), null, 34))
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(840, "Adreno840v2"), null, 34))
    }

    @Test
    fun anAdrenoWithNothingToGoOnKeepsTheOldFallback() {
        // Android 16 shipped with the 8 Elite, so a device whose kernel names no model is taken as 8xx.
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(0), null, 36))
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(0), null, 33))
        // A 6xx name falls through to that same fallback: only 7xx and 8xx are picked by generation
        // digit. That is how it behaves today and this asserts it stays a decision rather than
        // something that changes by accident.
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(610), null, 36))
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(610), null, 33))
    }

    @Test
    fun anAdrenoKeepsItsPinnedAndOverriddenBuild() {
        // Unchanged behaviour: a pin or an override wins over the generation on an Adreno, exactly as before.
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(750), "turnip25.1.0", 36))
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(830), "turnip-sdk36", 33))
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(0), "turnip-sdk36", 33))
        // A request that is not one of the bundled builds is ignored, and the GPU decides.
        assertEquals("turnip25.1.0", TurnipDriver.bundledDriverFor(adrenoGpu(750), "some-other.zip", 36))
        assertEquals("turnip-sdk36", TurnipDriver.bundledDriverFor(adrenoGpu(830), null, 33))
    }

    @Test
    fun theMaliBuildIsABundledBuildSoNoOtherPathCanSkipIt() {
        // The id is in BUNDLED, which is what the import list, the delete guard and the pin test all
        // read: a Mali build that ships is offered and protected like the Turnips, not treated as an
        // import. It is also the vendor test the pick and the menu both use.
        assertTrue(TurnipDriver.BUNDLED.contains(TurnipDriver.DRIVER_MALI_G715))
        assertTrue(TurnipDriver.isMaliBuild(TurnipDriver.DRIVER_MALI_G715))
        assertFalse(TurnipDriver.isMaliBuild("turnip25.1.0"))
        assertFalse(TurnipDriver.isMaliBuild(null))
    }
}
