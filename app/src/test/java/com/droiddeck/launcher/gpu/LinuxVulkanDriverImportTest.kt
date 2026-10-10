package com.droiddeck.launcher.gpu

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Importing a Linux (glibc) Vulkan driver for the runtime. The library is the vendor's - Turnip's
 * `libvulkan_freedreno.so` on an Adreno, PanVK's `libvulkan_panvk.so` on a Mali - and the manifest
 * the session is handed must name the file that is really on disk, or the guest loads nothing. Only
 * glibc builds belong here: a bionic one cannot be loaded by the runtime at all.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [28], manifest = Config.NONE)
class LinuxVulkanDriverImportTest {
    private lateinit var context: Context
    private lateinit var manager: LinuxVulkanDriverManager

    @Before fun setUp() {
        context = RuntimeEnvironment.getApplication()
        File(context.filesDir, LinuxVulkanDriverManager.DIR_NAME).deleteRecursively()
        manager = LinuxVulkanDriverManager(context)
    }

    /** Enough of an AArch64 ELF for the checks the importer makes: the header, then the libc it links. */
    private fun library(libc: String) = ByteArray(64).also {
        byteArrayOf(0x7f, 'E'.code.toByte(), 'L'.code.toByte(), 'F'.code.toByte(), 2, 1).copyInto(it)
        it[18] = 0xB7.toByte()
    } + libc.toByteArray() + ByteArray(1)

    private fun zip(entries: Map<String, ByteArray>): Uri {
        val file = File.createTempFile("driver", ".zip", context.cacheDir)
        ZipOutputStream(file.outputStream()).use { z ->
            for ((name, data) in entries) { z.putNextEntry(ZipEntry(name)); z.write(data); z.closeEntry() }
        }
        return Uri.fromFile(file)
    }

    private fun driverZip(libraryName: String, libc: String = "libc.so.6", name: String = "PanVK"): Uri = zip(mapOf(
        LinuxVulkanDriverManager.META_NAME to JSONObject().put("name", name).put("driverVersion", "25.1.0").toString().toByteArray(),
        libraryName to library(libc),
    ))

    private fun manifestOf(id: String) = JSONObject(File(manager.getIcdPath(id)).readText()).getJSONObject("ICD")

    @Test fun aPanVkZipIsKeptUnderItsOwnNameAndTheManifestPointsAtIt() {
        val id = manager.installDriver(driverZip("libvulkan_panvk.so"), "PanVK")
        // The library is stored under the name the zip gave it, not renamed to Turnip's.
        assertEquals("libvulkan_panvk.so", manager.libraryName(id))
        assertTrue(manager.isInstalled(id))
        assertEquals(listOf(id), manager.enumerateInstalledDrivers())
        assertEquals(
            File(manager.getDriverDir(id), "libvulkan_panvk.so").absolutePath,
            manifestOf(id).getString("library_path"),
        )
        // ... and the name is recorded, which is what libraryName reads back.
        val meta = JSONObject(File(manager.getDriverDir(id), LinuxVulkanDriverManager.META_NAME).readText())
        assertEquals("libvulkan_panvk.so", meta.getString("sourceLibraryName"))
    }

    @Test fun aTurnipZipIsStillImportedExactlyAsBefore() {
        val id = manager.installDriver(driverZip("libvulkan_freedreno.so", name = "Turnip 25.1"), "Turnip")
        assertEquals(LinuxVulkanDriverManager.LIB_NAME, manager.libraryName(id))
        assertTrue(manager.isInstalled(id))
        assertEquals(
            File(manager.getDriverDir(id), LinuxVulkanDriverManager.LIB_NAME).absolutePath,
            manifestOf(id).getString("library_path"),
        )
        assertEquals("Turnip 25.1", manager.getDriverName(id))
    }

    @Test fun aBionicDriverIsStillRefusedHere() {
        // The same zip's bionic half belongs in the display list; in the runtime it would draw nothing.
        try {
            manager.installDriver(driverZip("libvulkan_panvk.so", libc = "libc.so"), "PanVK")
            fail("a bionic library must be refused in the runtime list")
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!, e.message!!.contains("glibc"))
        }
        assertTrue(manager.enumerateInstalledDrivers().isEmpty())
    }

    @Test fun aZipWithNoVulkanLibraryIsRefusedByTheGeneralRule() {
        val uri = zip(mapOf(LinuxVulkanDriverManager.META_NAME to "{}".toByteArray(), "notadriver.so" to library("libc.so.6")))
        try {
            manager.installDriver(uri, "something")
            fail("a zip with no libvulkan_*.so must be refused")
        } catch (e: IllegalArgumentException) {
            assertTrue(e.message!!, e.message!!.contains("libvulkan_"))
        }
        assertTrue(manager.enumerateInstalledDrivers().isEmpty())
    }

    @Test fun anInstallFromBeforeTheNameWasKeptIsStillOffered() {
        // Every driver imported before the library kept its own name holds LIB_NAME; it must keep
        // working, and its library must be handed to the guest as it always was.
        val dir = manager.getDriverDir("legacy").apply { mkdirs() }
        File(dir, LinuxVulkanDriverManager.LIB_NAME).writeBytes(library("libc.so.6"))
        File(dir, LinuxVulkanDriverManager.ICD_NAME).writeText("{}")
        File(dir, LinuxVulkanDriverManager.META_NAME).writeText(JSONObject().put("name", "Old Turnip").toString())
        assertTrue(manager.isInstalled("legacy"))
        assertEquals(LinuxVulkanDriverManager.LIB_NAME, manager.libraryName("legacy"))
        assertEquals(listOf("legacy"), manager.enumerateInstalledDrivers())
        assertEquals("Old Turnip", manager.getDriverName("legacy"))
    }

    @Test fun aHalfExtractedDriverIsNeverOffered() {
        // The library without its manifest is not installed, and neither is the reverse.
        val dir = manager.getDriverDir("half").apply { mkdirs() }
        File(dir, "libvulkan_panvk.so").writeBytes(library("libc.so.6"))
        assertTrue(manager.enumerateInstalledDrivers().isEmpty())
        File(dir, LinuxVulkanDriverManager.ICD_NAME).writeText("{}")
        assertEquals(listOf("half"), manager.enumerateInstalledDrivers())
    }
}
