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
 * One call that gathers, as text, what a device looks like at an instant.
 *
 * The reason this exists: several sessions were diagnosed from a crash report plus whatever log
 * happened to survive, and the question "what did the display path actually look like at that
 * moment" could not be answered afterwards. Snapshot writes the answer where the session's other
 * artifacts go. It is text only - no image is taken, ever (see the project's no-screenshot rule).
 */
object AgentDiagnostics {
    private const val TAG = "AgentDiagnostics"

    private val PROBES = listOf(
        "dumpsys SurfaceFlinger" to "surfaceflinger.txt",
        "dumpsys display" to "display.txt",
        "dumpsys input" to "input.txt",
        "dumpsys window displays" to "windows.txt",
    )

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
        PROBES.forEach { (command, name) ->
            runCatching { File(out, name).writeText(shell(command)) }
                .onFailure { Log.w(TAG, "probe failed: $command", it) }
        }
        SessionEvents.record("diag.snapshot", mapOf("dir" to out.absolutePath), target)
        return out
    }

    /** A marker in the session's event stream, so a host action has a place on the timeline. */
    fun mark(context: Context, label: String): Boolean {
        SessionEvents.record("diag.mark", mapOf("label" to label))
        return true
    }

    private fun shell(command: String): String {
        val process = ProcessBuilder("/system/bin/sh", "-c", command)
            .redirectErrorStream(true).start()
        val text = process.inputStream.bufferedReader().readText()
        process.waitFor()
        return text
    }

    /** Every process holding a graphics library, with its role and the libraries it has mapped. */
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
            out.append("pid=").append(dir.name).append(" comm=").append(comm)
                .append(" role=").append(role).append('\n')
            libs.forEach { out.append("  lib ").append(it).append('\n') }
        }
        return out.toString()
    }

    private val INTERESTING = listOf("steam", "gamescope", "Xwayland", "steamwebhelper", "droiddeck")
    private val LIBS = listOf("libvulkan_powervr", "libhybris", "libgbm", "swiftshader")
}
