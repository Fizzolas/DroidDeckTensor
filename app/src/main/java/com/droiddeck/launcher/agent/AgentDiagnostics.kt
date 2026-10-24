package com.droiddeck.launcher.agent

import android.content.Context
import android.util.Log
import com.droiddeck.launcher.session.SessionEvents
import com.droiddeck.launcher.session.SessionPaths
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * What the app itself can say about the device at an instant, as text.
 *
 * The reason this exists: several sessions were diagnosed from a crash report plus whatever log
 * happened to survive, and the question "which binary had each process actually mapped" could not
 * be answered afterwards. A process's mapped inode is proof of a load; the installed file's hash is
 * not (state doc §36, §63).
 *
 * **Not here: `dumpsys`.** An app process holds no `android.permission.DUMP`, so every dumpsys
 * probe returns "Permission Denial" and writing that to a file that looks like evidence is worse
 * than not writing it. The host does hold it (adb shell's own DUMP grant), so DisplayManager,
 * SurfaceFlinger and input dumps are gathered host-side by `tools/diagkit/runs.py`, into the same
 * snapshot directory. This class stays within what the app is actually allowed to see.
 *
 * Text only - no image is taken, ever (the project's no-screenshot rule).
 */
object AgentDiagnostics {
    private const val TAG = "AgentDiagnostics"

    /** Writes a snapshot into [dir] (the session folder) and returns the directory it used. */
    @Synchronized
    fun snapshot(context: Context, dir: File?): File {
        val target = dir ?: SessionPaths.current()
            ?: File(context.filesDir, "diag-snapshots").apply { mkdirs() }
        target.mkdirs()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val out = File(target, "snapshot-$stamp")
        out.mkdirs()
        File(out, "procs.txt").writeText(processTable())
        File(out, "app.txt").writeText(appState(context))
        SessionEvents.record("diag.snapshot", mapOf("dir" to out.absolutePath), target)
        return out
    }

    /** A marker in the session's event stream, so a host action has a place on the timeline. */
    fun mark(context: Context, label: String): Boolean {
        SessionEvents.record("diag.mark", mapOf("label" to label))
        return true
    }

    private fun appState(context: Context): String = buildString {
        append("package=").append(context.packageName).append('\n')
        runCatching {
            val info = context.packageManager.getPackageInfo(context.packageName, 0)
            append("versionName=").append(info.versionName).append('\n')
            @Suppress("DEPRECATION")
            append("versionCode=").append(info.versionCode).append('\n')
        }.onFailure { Log.w(TAG, "package info", it) }
        append("firstApiLevel=").append(android.os.Build.VERSION.SDK_INT).append('\n')
        append("fingerprint=").append(android.os.Build.FINGERPRINT).append('\n')
    }

    /**
     * Every process holding a graphics library, with its role and the libraries it has mapped.
     * This is the field an installed-file hash cannot give.
     */
    private fun processTable(): String {
        val out = StringBuilder()
        val dirs = File("/proc").listFiles()?.filter { it.name.all(Char::isDigit) } ?: return ""
        for (dir in dirs.sortedBy { it.name }) {
            val cmdline = runCatching { File(dir, "cmdline").readText() }.getOrNull() ?: continue
            if (!INTERESTING.any { cmdline.contains(it) }) continue
            val comm = runCatching { File(dir, "comm").readText().trim() }.getOrNull() ?: "?"
            val role = when {
                cmdline.contains("--type=gpu-process") -> "gpu"
                cmdline.contains("--type=renderer") -> "renderer"
                cmdline.contains("--type=zygote") -> "zygote"
                cmdline.contains("--type=utility") -> "utility"
                else -> "browser"
            }
            val maps = runCatching { File(dir, "maps").readLines() }.getOrNull() ?: emptyList()
            val libs = maps.filter { line -> LIBS.any { line.contains(it) } }
                .map { it.substringAfterLast(' ') }
                .distinct()
                .filter { it.isNotBlank() }
            out.append("pid=").append(dir.name).append(" comm=").append(comm)
                .append(" role=").append(role).append('\n')
            libs.forEach { out.append("  lib ").append(it).append('\n') }
        }
        return out.toString()
    }

    private val INTERESTING = listOf("steam", "gamescope", "Xwayland", "steamwebhelper", "droiddeck")
    private val LIBS = listOf("libvulkan_powervr", "libhybris", "libgbm", "swiftshader")
}
