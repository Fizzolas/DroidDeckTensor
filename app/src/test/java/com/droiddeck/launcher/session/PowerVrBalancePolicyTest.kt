package com.droiddeck.launcher.session

import android.content.Context
import com.droiddeck.launcher.gpu.GpuInfo.Vendor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class PowerVrBalancePolicyTest {
    private lateinit var context: Context

    @Before fun setUp() {
        context = RuntimeEnvironment.getApplication()
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit().clear().commit()
    }

    @Test fun vendorDefaultFpsLimitIsBalancedOnlyOnPowerVr() {
        assertEquals(60, SessionPrefs.defaultFpsLimitForVendor(Vendor.POWERVR))
        assertEquals(0, SessionPrefs.defaultFpsLimitForVendor(Vendor.ADRENO))
        assertEquals(0, SessionPrefs.defaultFpsLimitForVendor(Vendor.MALI))
        assertEquals(0, SessionPrefs.defaultFpsLimitForVendor(Vendor.OTHER))
    }

    @Test fun explicitUserFpsChoiceIncludingZeroOverridesDefault() {
        assertEquals(0, SessionPrefs.fpsLimit(context, SessionService.MODE_STEAM))
        SessionPrefs.setFpsLimit(context, SessionService.MODE_STEAM, 40)
        assertEquals(40, SessionPrefs.fpsLimit(context, SessionService.MODE_STEAM))
        SessionPrefs.setFpsLimit(context, SessionService.MODE_STEAM, 0)
        assertEquals(0, SessionPrefs.fpsLimit(context, SessionService.MODE_STEAM))
        assertTrue(context.getSharedPreferences("session", Context.MODE_PRIVATE).contains("fpsLimit.${SessionService.MODE_STEAM}"))
    }

    @Test fun thermalGovernorSteppedPacingAndHysteresisOnPowerVr() {
        val gov = ThermalGovernor(Vendor.POWERVR)

        // Cool startup keeps peak boost unlocked and honors requested cap (e.g. uncapped 0 or 60).
        val nominal = gov.update(baseFpsLimit = 0, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_NONE, activityVisible = true, screenOn = true)
        assertEquals(0, nominal.effectiveFpsLimit)
        assertFalse(nominal.sustainedPerformance)

        // Rising through MODERATE caps uncapped/>60 targets gently to 60 without locking sustained clocks yet.
        val modRising = gov.update(baseFpsLimit = 120, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_MODERATE, activityVisible = true, screenOn = true)
        assertEquals(60, modRising.effectiveFpsLimit)
        assertFalse(modRising.sustainedPerformance)

        // Hitting SEVERE engages sustained clocks + exact 3:1 integer divisor (40 fps on 120 Hz).
        val severe = gov.update(baseFpsLimit = 60, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_SEVERE, activityVisible = true, screenOn = true)
        assertEquals(40, severe.effectiveFpsLimit)
        assertTrue(severe.sustainedPerformance)

        // Cooling back into MODERATE retains SEVERE tier (hysteresis) until thermal status drops to LIGHT/NONE.
        val cooldownMod = gov.update(baseFpsLimit = 60, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_MODERATE, activityVisible = true, screenOn = true)
        assertEquals(40, cooldownMod.effectiveFpsLimit)
        assertTrue(cooldownMod.sustainedPerformance)

        // Once cooled to LIGHT, releases sustained mode and restores user's 60 fps cap.
        val cooled = gov.update(baseFpsLimit = 60, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_LIGHT, activityVisible = true, screenOn = true)
        assertEquals(60, cooled.effectiveFpsLimit)
        assertFalse(cooled.sustainedPerformance)

        // CRITICAL clamps to 30 fps on 120 Hz/60 Hz and never raises a lower user cap (e.g. 24/30).
        val critical = gov.update(baseFpsLimit = 60, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_CRITICAL, activityVisible = true, screenOn = true)
        assertEquals(30, critical.effectiveFpsLimit)
        assertTrue(critical.sustainedPerformance)

        // 60 Hz panel picks exact 30 fps under SEVERE; 90 Hz panel picks exact 45 fps under SEVERE.
        assertEquals(30, ThermalGovernor(Vendor.POWERVR).update(0, 60f, ThermalGovernor.STATUS_SEVERE, true, true).effectiveFpsLimit)
        assertEquals(45, ThermalGovernor(Vendor.POWERVR).update(0, 90f, ThermalGovernor.STATUS_SEVERE, true, true).effectiveFpsLimit)

        // Hidden / screen-off cuts background pacing to 15 fps.
        val hidden = ThermalGovernor(Vendor.POWERVR).update(60, 120f, ThermalGovernor.STATUS_NONE, activityVisible = false, screenOn = true)
        assertEquals(15, hidden.effectiveFpsLimit)
        assertFalse(hidden.sustainedPerformance)
    }

    @Test fun thermalGovernorLeavesAdrenoCompletelyUntouched() {
        val gov = ThermalGovernor(Vendor.ADRENO)
        val p = gov.update(baseFpsLimit = 0, panelRefreshHz = 120f, thermalStatus = ThermalGovernor.STATUS_CRITICAL, activityVisible = false, screenOn = false)
        assertEquals(0, p.effectiveFpsLimit)
        assertFalse(p.sustainedPerformance)
    }

    @Test fun powerVrGuestEnvPairsThreadQueueLatencyAndFrameRate() {
        val capped = SessionService.powerVrGuestEnv(fpsLimit = 60, fexPreset = "PERFORMANCE_TSO")
        val map = capped.associate { it.substringBefore('=') to it.substringAfter('=') }
        assertEquals("norp", map["ZINK_DEBUG"])
        assertEquals("0", map["GALLIUM_THREAD"])
        assertEquals("false", map["mesa_glthread"])
        assertEquals("60", map["DXVK_FRAME_RATE"])
        assertEquals("60", map["VKD3D_FRAME_RATE"])
        assertEquals("none", map["FEX_SMCCHECKS"])
        val dxvk = map.getValue("DXVK_CONFIG")
        assertTrue(dxvk.contains("dxvk.numCompilerThreads = 3"))
        assertTrue(dxvk.contains("dxgi.maxFrameLatency = 2"))
        assertTrue(dxvk.contains("d3d9.maxFrameLatency = 2"))

        val uncapped = SessionService.powerVrGuestEnv(fpsLimit = 0, fexPreset = "COMPATIBILITY")
            .associate { it.substringBefore('=') to it.substringAfter('=') }
        assertFalse(uncapped.containsKey("DXVK_FRAME_RATE"))
        assertFalse(uncapped.containsKey("VKD3D_FRAME_RATE"))
        assertFalse(uncapped.containsKey("FEX_SMCCHECKS"))
    }
}
