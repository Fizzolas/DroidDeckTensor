package com.droiddeck.launcher.runtime

import android.content.Context
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File

/**
 * Which Vulkan driver inside the runtime the session is pointed at. The runtime's own ICD manifests
 * are named for the stack they were built against - `freedreno_icd.aarch64.json` for Mesa's Turnip,
 * `panvk_icd.aarch64.json` for Arm's PanVK - and preferring freedreno on a Mali is one of the ways a
 * Tensor device ends up loading a driver that cannot open its GPU. The vendor is a parameter so the
 * choice is decided here, without a device.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class LinuxRuntimeIcdTest {
    private lateinit var context: Context

    @Before fun setUp() {
        context = RuntimeEnvironment.getApplication()
        File(LinuxRuntime.rootDir(context), "usr/share/vulkan").deleteRecursively()
    }

    /** The runtime's ICD directory with exactly these manifests in it. */
    private fun manifests(vararg names: String) {
        val dir = File(LinuxRuntime.rootDir(context), "usr/share/vulkan/icd.d")
        dir.deleteRecursively()
        dir.mkdirs()
        for (name in names) File(dir, name).writeText("{}")
    }

    @Test fun theManifestChosenIsTheOneBuiltForTheDevicesStack() {
        manifests("freedreno_icd.aarch64.json", "panvk_icd.aarch64.json")
        assertEquals("panvk_icd.aarch64.json", LinuxRuntime.vulkanIcd(context, true)?.name)
        assertEquals("freedreno_icd.aarch64.json", LinuxRuntime.vulkanIcd(context, false)?.name)
    }

    @Test fun aRuntimeWithOnlyTheOtherStacksDriverStillResolvesToIt() {
        // Every runtime built so far ships freedreno. A Mali must resolve to that rather than to
        // nothing: the session then fails honestly, instead of the app claiming a driver it has not got.
        manifests("freedreno_icd.aarch64.json")
        assertEquals("freedreno_icd.aarch64.json", LinuxRuntime.vulkanIcd(context, true)?.name)
    }

    @Test fun anyManifestBeatsNoneAndNoManifestIsNothing() {
        manifests("something_else.json")
        assertEquals("something_else.json", LinuxRuntime.vulkanIcd(context, false)?.name)
        assertEquals("something_else.json", LinuxRuntime.vulkanIcd(context, true)?.name)
        manifests()
        assertNull(LinuxRuntime.vulkanIcd(context, true))
        assertNull(LinuxRuntime.vulkanIcd(context, false))
    }

    @Test fun theManifestChosenForAPowerVrIsItsOwnStacks() {
        manifests("freedreno_icd.aarch64.json", "panvk_icd.aarch64.json", "powervr_icd.aarch64.json")
        assertEquals("powervr_icd.aarch64.json",
            LinuxRuntime.vulkanIcd(context, com.droiddeck.launcher.gpu.GpuInfo.Vendor.POWERVR)?.name)
    }

    @Test fun theRuntimesOwnDriverIsNamedForTheVendor() {
        // One owner for the names three places agree on: the ICD discovery, the Flatpak GPU plan and
        // the library the sandbox binds.
        assertEquals("freedreno", LinuxRuntime.runtimeVulkanVendor(false))
        assertEquals("panvk", LinuxRuntime.runtimeVulkanVendor(true))
        assertEquals("libvulkan_freedreno.so", LinuxRuntime.runtimeVulkanLibrary(false))
        assertEquals("libvulkan_panvk.so", LinuxRuntime.runtimeVulkanLibrary(true))
        // A Tensor G5's PowerVR gets its own name, not a Turnip it could not load.
        assertEquals("powervr", LinuxRuntime.runtimeVulkanVendor(com.droiddeck.launcher.gpu.GpuInfo.Vendor.POWERVR))
        assertEquals("libvulkan_powervr.so",
            LinuxRuntime.runtimeVulkanLibrary(com.droiddeck.launcher.gpu.GpuInfo.Vendor.POWERVR))
    }
}
