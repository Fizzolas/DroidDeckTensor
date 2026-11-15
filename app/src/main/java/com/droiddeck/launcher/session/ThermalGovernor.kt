package com.droiddeck.launcher.session

import com.droiddeck.launcher.gpu.GpuInfo

/**
 * Adaptive thermal and background frame-pacing governor for PowerVR (Tensor G5) sessions.
 *
 * Unlike static `SustainedPerformanceMode` at boot (which caps peak clocks needed for fast shader
 * loads), this keeps boost headroom available while the device is cool and intervenes only when
 * Android reports sustained thermal pressure or when the session is running hidden/screen-off:
 *
 *  - `NONE` / `LIGHT` (0..1): keeps user/session [baseFpsLimit] with sustained mode off.
 *  - `MODERATE` (2): caps uncapped or >60 targets to 60 fps (held at the `SEVERE` tier on the way
 *    down until cooling reaches `LIGHT` so clocks and frame caps do not oscillate).
 *  - `SEVERE` (3): engages Android sustained performance mode and steps down to the fastest
 *    integer-divisible panel cadence <= 48 fps (40 fps on 120 Hz, 45 on 90 Hz, 30 on 60 Hz).
 *  - `CRITICAL+` (>= 4): keeps sustained performance mode and steps down to 30 fps (36 on 144 Hz).
 *  - Hidden / screen-off (`!activityVisible || !screenOn`): paces down to 15 fps.
 *
 * Always bypasses non-PowerVR vendors so Adreno/Mali sessions remain untouched.
 */
class ThermalGovernor(private val vendor: GpuInfo.Vendor = GpuInfo.detect().family.vendor) {

    data class Policy(
        val effectiveFpsLimit: Int,
        val sustainedPerformance: Boolean,
        val reason: String,
    )

    private var sustainedActive = false

    fun update(
        baseFpsLimit: Int,
        panelRefreshHz: Float,
        thermalStatus: Int,
        activityVisible: Boolean,
        screenOn: Boolean,
    ): Policy {
        if (vendor != GpuInfo.Vendor.POWERVR) {
            return Policy(baseFpsLimit, sustainedPerformance = false, reason = "non-powervr")
        }

        if (thermalStatus >= STATUS_SEVERE) {
            sustainedActive = true
        } else if (thermalStatus <= STATUS_LIGHT) {
            sustainedActive = false
        }

        if (!activityVisible || !screenOn) {
            return Policy(
                effectiveFpsLimit = minPositive(baseFpsLimit, BACKGROUND_FPS_CAP),
                sustainedPerformance = false,
                reason = if (!screenOn) "screen-off" else "background",
            )
        }

        return when {
            thermalStatus >= STATUS_CRITICAL -> Policy(
                effectiveFpsLimit = minPositive(baseFpsLimit, pickCadenceCap(panelRefreshHz, CRITICAL_CAPS, 30)),
                sustainedPerformance = true,
                reason = "thermal-critical($thermalStatus)",
            )
            thermalStatus >= STATUS_SEVERE || (thermalStatus == STATUS_MODERATE && sustainedActive) -> Policy(
                effectiveFpsLimit = minPositive(baseFpsLimit, pickCadenceCap(panelRefreshHz, SEVERE_CAPS, 30)),
                sustainedPerformance = true,
                reason = if (thermalStatus >= STATUS_SEVERE) "thermal-severe($thermalStatus)" else "thermal-cooldown($thermalStatus)",
            )
            thermalStatus == STATUS_MODERATE -> Policy(
                effectiveFpsLimit = minPositive(baseFpsLimit, MODERATE_FPS_CAP),
                sustainedPerformance = false,
                reason = "thermal-moderate($thermalStatus)",
            )
            else -> Policy(
                effectiveFpsLimit = baseFpsLimit,
                sustainedPerformance = false,
                reason = "nominal($thermalStatus)",
            )
        }
    }

    companion object {
        const val STATUS_NONE = 0
        const val STATUS_LIGHT = 1
        const val STATUS_MODERATE = 2
        const val STATUS_SEVERE = 3
        const val STATUS_CRITICAL = 4

        private const val BACKGROUND_FPS_CAP = 15
        private const val MODERATE_FPS_CAP = 60
        private val SEVERE_CAPS = intArrayOf(48, 45, 40, 36, 30)
        private val CRITICAL_CAPS = intArrayOf(36, 30)

        /** Never raises the user's explicit cap: 0 means uncapped, so any positive cap wins. */
        internal fun minPositive(baseCap: Int, limit: Int): Int =
            if (baseCap in 1..limit) baseCap else limit

        internal fun pickCadenceCap(panelRefreshHz: Float, candidates: IntArray, fallback: Int): Int {
            for (fps in candidates) {
                if (PerfMode.cadenceFits(panelRefreshHz, fps)) return fps
            }
            return fallback
        }
    }
}
