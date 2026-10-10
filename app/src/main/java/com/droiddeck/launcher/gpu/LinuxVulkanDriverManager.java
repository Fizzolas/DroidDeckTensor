package com.droiddeck.launcher.gpu;

import android.content.Context;
import android.net.Uri;
import android.util.Log;

import com.droiddeck.launcher.R;
import com.droiddeck.launcher.core.FileUtils;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Imported LINUX Vulkan drivers: glibc Vulkan ICDs for the runtime - the gamescope session that
 * runs Valve's native ARM64 Steam
 * client, and the labwc desktop beside it. This is the driver that DRAWS there: the client's own UI
 * (OpenGL through the runtime's Zink), every game the client launches (D3D through Proton's
 * DXVK/VKD3D) and everything on the desktop. Putting the frame on the screen stays the Android
 * driver's job, in the app's compositor ({@link TurnipDriver}).
 *
 * <p>Not interchangeable with that one: the client and its games are glibc processes and cannot
 * load a bionic object at all, and this one cannot be loaded by the app. The check that separates
 * them is the libc the driver links against - {@code libc.so.6} here, {@code libc.so} there.
 *
 * <p>Layout (under the app's files dir, which the session sees by its full host path):
 * <pre>
 *   files/linux_vulkan_drivers/&lt;id&gt;/
 *       libvulkan_&lt;vendor&gt;.so   the driver, under the name its zip gave it (Turnip, PanVK, ...)
 *       icd.json                 generated Vulkan ICD manifest; library_path = the .so's ABSOLUTE path
 *       meta.json                name / driverVersion / sourceLibraryName / minGlibc, our own schema
 * </pre>
 * The session gets {@code icd.json}'s path in {@code BL_VK_DRIVER} and exports it as
 * {@code VK_DRIVER_FILES}, so nothing inside the runtime is modified and the choice is reversible.
 *
 * <p>Ported from Bannerlator's {@code LinuxVulkanDriverManager} (GPL-3.0), unchanged in behaviour.
 */
public class LinuxVulkanDriverManager {
    private static final String TAG = "LinuxVulkanDriver";
    public static final String DIR_NAME = "linux_vulkan_drivers";
    /**
     * The library name an install from before the zip's own name was kept carries (the Turnip name
     * every such zip uses); a driver is found by {@link #libraryName} through this and its meta.
     */
    public static final String LIB_NAME = "libvulkan_freedreno.so";
    /**
     * What a glibc Vulkan driver's library is called, whatever vendor built it: Turnip names it
     * {@code libvulkan_freedreno.so}, PanVK {@code libvulkan_panvk.so}. Naming the vendor here was
     * what kept a Mali's driver out of the runtime list (see the Tensor work doc, item 8).
     */
    private static final String LIB_PREFIX = "libvulkan_";
    public static final String ICD_NAME = "icd.json";
    public static final String META_NAME = "meta.json";

    private final Context context;
    private final File rootDir;

    public LinuxVulkanDriverManager(Context context) {
        this.context = context;
        this.rootDir = new File(context.getFilesDir(), DIR_NAME);
        if (!rootDir.exists()) rootDir.mkdirs();
    }

    public File getDriverDir(String id) {
        return new File(rootDir, id);
    }

    /** Installed = has the library AND the generated manifest (a half-extracted dir is never offered). */
    public boolean isInstalled(String id) {
        if (id == null || id.isEmpty() || id.contains("/") || id.contains("..")) return false;
        return libraryName(id) != null && new File(getDriverDir(id), ICD_NAME).isFile();
    }

    /**
     * The library a driver's directory holds, or null when it holds none: the name its own meta
     * recorded (a PanVK keeps {@code libvulkan_panvk.so}, a Turnip {@code libvulkan_freedreno.so}),
     * {@link #LIB_NAME} for an install from before that name was kept, or any
     * {@code libvulkan_*.so} as a last resort.
     */
    public String libraryName(String id) {
        if (id == null || id.isEmpty() || id.contains("/") || id.contains("..")) return null;
        File dir = getDriverDir(id);
        JSONObject meta = readMeta(id);
        String recorded = meta != null ? meta.optString("sourceLibraryName", "") : "";
        if (!recorded.isEmpty() && new File(dir, recorded).isFile()) return recorded;
        if (new File(dir, LIB_NAME).isFile()) return LIB_NAME;
        File[] libraries = dir.listFiles((d, name) -> name.startsWith(LIB_PREFIX) && name.endsWith(".so"));
        return libraries != null && libraries.length > 0 ? libraries[0].getName() : null;
    }

    /** Absolute path of the driver's ICD manifest, or null when the id isn't installed. */
    public String getIcdPath(String id) {
        return isInstalled(id) ? new File(getDriverDir(id), ICD_NAME).getAbsolutePath() : null;
    }

    public List<String> enumerateInstalledDrivers() {
        ArrayList<String> ids = new ArrayList<>();
        File[] dirs = rootDir.listFiles();
        if (dirs == null) return ids;
        for (File d : dirs) if (d.isDirectory() && isInstalled(d.getName())) ids.add(d.getName());
        Collections.sort(ids);
        return ids;
    }

    private JSONObject readMeta(String id) {
        try {
            File meta = new File(getDriverDir(id), META_NAME);
            if (!meta.isFile()) return null;
            return new JSONObject(FileUtils.readString(meta));
        } catch (Exception e) {
            return null;
        }
    }

    public String getDriverName(String id) {
        JSONObject m = readMeta(id);
        String name = m != null ? m.optString("name", "") : "";
        return name.isEmpty() ? id : name;
    }

    public String getDriverVersion(String id) {
        JSONObject m = readMeta(id);
        return m != null ? m.optString("driverVersion", "") : "";
    }

    /** The glibc the driver asks for, as its zip recorded it ("" when the zip did not say). */
    public String getMinGlibc(String id) {
        JSONObject m = readMeta(id);
        return m != null ? m.optString("minGlibc", "") : "";
    }

    public void removeDriver(String id) {
        if (id == null || id.isEmpty() || id.contains("/") || id.contains("..")) return;
        Log.d(TAG, "removing imported Linux Vulkan driver " + id);
        FileUtils.delete(getDriverDir(id));
        // A mode still set to this id falls back to the runtime's own driver at launch (logged there).
    }

    /**
     * Import a {@code -Linux.zip}. Returns the new driver id.
     * @throws IllegalArgumentException with a user-facing reason when the zip is not an importable
     *         Linux driver; {@link IOException} on read/extract failures.
     */
    public String installDriver(Uri zipUri, String displayName) throws IOException {
        File tmpDir = newStagingDir();
        boolean keep = false;
        try {
            String soName = null;
            JSONObject zipMeta = null;
            try (InputStream is = context.getContentResolver().openInputStream(zipUri);
                 ZipInputStream zis = new ZipInputStream(is)) {
                if (is == null) throw new IOException("cannot open " + zipUri);
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    if (entry.isDirectory()) continue;
                    // Flatten: only the base name matters, and it also defeats zip-slip paths.
                    String base = new File(entry.getName()).getName();
                    if (base.isEmpty()) continue;
                    if (base.startsWith(LIB_PREFIX) && base.endsWith(".so")) {
                        // The primary library is the first libvulkan_*.so; the manifest points at it.
                        if (soName == null) soName = base;
                        else Log.w(TAG, "zip has several " + LIB_PREFIX + "*.so; using the first (" + soName + ")");
                    } else if (base.equals(META_NAME)) {
                        try {
                            zipMeta = new JSONObject(new String(readAll(zis), StandardCharsets.UTF_8));
                        } catch (Exception e) {
                            Log.w(TAG, "meta.json unreadable, ignoring: " + e.getMessage());
                        }
                        continue;
                    } else if (base.equals(ICD_NAME) || base.endsWith(".json")) {
                        // The zip's own ICD manifest is dropped: its library_path is relative to
                        // itself, and adopt() writes an absolute one.
                        continue;
                    }
                    // Every other file is kept: a glibc driver may need support libraries beside it
                    // (a shim layer, a client library), and it finds them relative to itself. Flatten
                    // to the base name to defeat zip-slip paths.
                    Files.copy(zis, new File(tmpDir, base).toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
            }
            String id = adopt(tmpDir, soName, zipMeta, displayName);
            keep = true;
            return id;
        } finally {
            if (!keep) FileUtils.delete(tmpDir);
        }
    }

    /** An empty directory beside the installed drivers, for a driver's files before {@link #adopt}. */
    File newStagingDir() throws IOException {
        File tmpDir = new File(rootDir, ".tmp-" + System.nanoTime());
        FileUtils.delete(tmpDir);
        if (!tmpDir.mkdirs()) throw new IOException("cannot create " + tmpDir);
        return tmpDir;
    }

    /**
     * Check a staged driver ({@code soName} in {@code tmpDir}), write its manifest and meta.json,
     * and move it in. Returns the id; the caller deletes {@code tmpDir} when this throws.
     */
    String adopt(File tmpDir, String soName, JSONObject zipMeta, String displayName) throws IOException {
        try {
            if (soName == null) {
                throw new IllegalArgumentException(context.getString(R.string.lvd_no_library));
            }
            File so = new File(tmpDir, soName);
            if (!isAarch64Elf(so)) {
                throw new IllegalArgumentException(context.getString(R.string.lvd_not_aarch64, soName));
            }
            // The one check that actually separates this from the bionic builds. Both sonames
            // are in the driver's .dynstr, so a byte scan is enough and needs no ELF parsing:
            // glibc's is versioned ("libc.so.6"), bionic's is not ("libc.so").
            if (!containsAscii(so, "libc.so.6")) {
                throw new IllegalArgumentException(context.getString(R.string.lvd_not_glibc, soName));
            }
            String kind = zipMeta != null ? zipMeta.optString("kind", "") : "";
            if (!kind.isEmpty() && !"linux-vulkan-icd".equals(kind)) {
                Log.w(TAG, "meta.json says kind=" + kind + ", importing anyway (the binary is glibc)");
            }

            String name = zipMeta != null ? zipMeta.optString("name", "") : "";
            String version = zipMeta != null ? zipMeta.optString("driverVersion", "") : "";
            String minGlibc = zipMeta != null ? zipMeta.optString("minGlibc", "") : "";
            if (name.isEmpty()) {
                name = displayName != null ? displayName : "linux-driver";
                if (name.toLowerCase().endsWith(".zip")) name = name.substring(0, name.length() - 4);
            }
            String id = uniqueId(sanitizeId(name));
            File dir = getDriverDir(id);

            // ICD manifest: absolute library_path, so the loader never depends on cwd and the guest
            // reads it at the same path the app wrote (the files dir is bound into the session).
            JSONObject icd = new JSONObject();
            icd.put("file_format_version", "1.0.0");
            JSONObject icdBody = new JSONObject();
            icdBody.put("library_path", new File(dir, soName).getAbsolutePath());
            icdBody.put("api_version", "1.1.274");
            icd.put("ICD", icdBody);
            if (!FileUtils.writeString(new File(tmpDir, ICD_NAME), icd.toString(2))) throw new IOException("cannot write icd.json");

            JSONObject meta = new JSONObject();
            meta.put("schemaVersion", 1);
            meta.put("kind", "linux-vulkan-icd");
            meta.put("name", name);
            meta.put("driverVersion", version);
            meta.put("libc", "glibc");
            meta.put("minGlibc", minGlibc);
            meta.put("sourceLibraryName", soName);
            meta.put("importedAt", System.currentTimeMillis());
            if (!FileUtils.writeString(new File(tmpDir, META_NAME), meta.toString(2))) throw new IOException("cannot write meta.json");

            if (!tmpDir.renameTo(dir)) throw new IOException("cannot move into " + dir);
            Log.i(TAG, "imported Linux Vulkan driver " + id + " (" + soName + ", minGlibc=" + minGlibc + ") -> " + dir);
            return id;
        } catch (org.json.JSONException e) {
            throw new IOException("manifest write failed: " + e.getMessage());
        }
    }

    private String uniqueId(String base) {
        String id = base;
        int n = 2;
        while (getDriverDir(id).exists()) id = base + "-" + (n++);
        return id;
    }

    static String sanitizeId(String name) {
        String s = name.trim().replaceAll("[^A-Za-z0-9._-]+", "_").replaceAll("^[._]+", "");
        if (s.length() > 64) s = s.substring(0, 64);
        return s.isEmpty() ? "linux-driver" : s;
    }

    /** ELF magic, EI_CLASS = 64-bit, EI_DATA = little-endian, e_machine = 0xB7 (AArch64). */
    static boolean isAarch64Elf(File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] h = new byte[20];
            int n = 0;
            while (n < h.length) {
                int r = in.read(h, n, h.length - n);
                if (r < 0) break;
                n += r;
            }
            if (n < 20) return false;
            if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return false;
            if (h[4] != 2 || h[5] != 1) return false;
            int machine = (h[18] & 0xff) | ((h[19] & 0xff) << 8);
            return machine == 0xB7;
        } catch (IOException e) {
            return false;
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int r;
        while ((r = in.read(buf)) > 0) bos.write(buf, 0, r);
        return bos.toByteArray();
    }

    static boolean containsAscii(File f, String needle) {
        byte[] nb = needle.getBytes(StandardCharsets.US_ASCII);
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            int carry = 0;
            int r;
            while ((r = in.read(buf, carry, buf.length - carry)) > 0) {
                int len = carry + r;
                for (int i = 0; i + nb.length <= len; i++) {
                    int j = 0;
                    while (j < nb.length && buf[i + j] == nb[j]) j++;
                    if (j == nb.length) return true;
                }
                // Keep the last needle-1 bytes so a match across a buffer boundary is still found.
                carry = Math.min(nb.length - 1, len);
                System.arraycopy(buf, len - carry, buf, 0, carry);
            }
        } catch (IOException e) {
            return false;
        }
        return false;
    }
}
