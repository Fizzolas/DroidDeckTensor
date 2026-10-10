package com.droiddeck.launcher.gpu

import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

/**
 * What the apk actually carries decides what can be offered. The two Adreno Turnips ship with the app;
 * the Mali build does not exist yet (the blocker), and this is where that is enforced rather than
 * assumed: no archive, no listing, no pick, no unpacking - so a Tensor device falls back to the system
 * Vulkan driver instead of being handed a driver that is not there.
 *
 * When the Mali archive is finally built and dropped into `assets/graphics_driver/`, this test is
 * meant to fail: that is the signal to say out loud that the driver ships, and to update here.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class TurnipDriverArchiveTest {
    private val context: Context = RuntimeEnvironment.getApplication()

    @Test fun onlyTheArchivesTheApkCarriesAreOffered() {
        val driver = TurnipDriver(context)
        driver.restoreBundled()

        assertTrue(driver.carried("turnip25.1.0"))
        assertTrue(driver.carried("turnip-sdk36"))
        assertFalse("the Mali archive is the blocker: no build carries one yet", driver.carried(TurnipDriver.DRIVER_MALI_G715))

        // ... so the display-driver list is the two Turnips, never the un-backed Mali id.
        assertEquals(listOf("turnip25.1.0", "turnip-sdk36"), driver.visibleBundled())
    }
}
