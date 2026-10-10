package com.droiddeck.launcher.gpu;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.util.Log;

import com.droiddeck.launcher.R;
import com.droiddeck.launcher.core.FileUtils;
import com.droiddeck.launcher.core.TarZst;
import com.droiddeck.launcher.session.SessionPrefs;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The Vulkan driver the in-app compositor runs on, and it has to be one built for this device's own
 * stack: on an Adreno a Turnip - the system Adreno driver does not implement
 * VK_EXT_image_drm_format_modifier, so importing the dma-bufs gamescope hands over fails and the
 * session renders nothing - and on a Mali Arm's PanVK on kbase. Two Turnips ship in the apk, one per
 * Adreno generation, and the GPU decides which is unpacked - unless the user has imported an
 * AdrenoTools zip of their own and chosen it, which then wins.
 *
 * <p>On a Mali - a Google Tensor phone, Arm's kbase instead of KGSL - an Adreno Turnip has no KGSL to
 * hook, so the pick is Arm's own driver ({@link #DRIVER_MALI_G715}) or, while no build carries one,
 * null: the compositor then falls back to the phone's own system Vulkan driver rather than being
 * handed a driver for a GPU the phone does not have. A GPU of neither vendor gets nothing.
 *
 * <p>An id is only ever offered when the apk actually carries its archive ({@link #carried}), so a
 * driver this build would have to bundle and does not degrades to the system driver with the missing
 * archive named in the log, instead of failing when a session starts.
 *
 * <p>The guest's own Turnip is a different copy entirely - a glibc build inside the rootfs, or an
 * imported one ({@link LinuxVulkanDriverManager}). This one is the bionic build the app process
 * loads through adrenotools, and it is what every session ends in: whatever draws inside the
 * runtime, this is the driver that puts the frame on the panel.
 *
 * <p>Every driver, bundled or imported, lives under {@code files/graphics_driver/<id>/} with a
 * {@code meta.json} whose {@code libraryName} names the .so beside it - the AdrenoTools layout.
 * The import path is ported from Bannerlator's {@code AdrenotoolsManager} (GPL-3.0), including
 * the refusal of zips that belong in the Linux runtime list: a "-Linux" (glibc) Turnip names no
 * library on purpose, and installed here it would put the compositor on the system Vulkan - a
 * black screen wearing a driver's name.
 */
public final class TurnipDriver {
    private static final String TAG = "TurnipDriver";
    /** Adreno 7xx (8 Gen 2/3, e.g. the Pocket FIT's A750). */
    private static final String DRIVER_A7XX = "turnip25.1.0";
    /** Adreno 8xx (8 Elite, e.g. a Fold 8's A830). */
    private static final String DRIVER_A8XX = "turnip-sdk36";
    /**
     * Arm's Valhall - a Google Tensor's Mali-G715 - drawn by Mesa's PanVK on the kbase kernel
     * driver, packaged like a Turnip (bionic, the AdrenoTools layout). The id is the contract with
     * the archive that would carry it: {@code graphics_driver/adrenotools-panvk-g715.tzst}.
     *
     * <p><b>No build carries that archive yet</b> (see the Tensor work doc: the driver is the
     * project's critical path), so {@link #carried} is false for it and nothing offers it - a Mali
     * session still gets the system Vulkan driver, and the missing archive is named in the log.
     * Dropping the archive into {@code assets/graphics_driver/} is all it takes to arm this pick.
     */
    static final String DRIVER_MALI_G715 = "panvk-g715";
    /**
     * The builds this app bundles by id: the two Adreno Turnips, and the Mali PanVK. Never
     * removable and unpacked on demand; which of them a given apk really has is its own archive's
     * business, and only {@link #carried} decides that.
     */
    public static final List<String> BUNDLED = Collections.unmodifiableList(Arrays.asList(DRIVER_A7XX, DRIVER_A8XX, DRIVER_MALI_G715));
    /** Where the bundled drivers' archives live in the apk, as {@code adrenotools-<id>.tzst}. */
    private static final String ASSET_DIR = "graphics_driver/";
    /** Overrides the pick, for a device we cannot reach: a7xx, a8xx or system. */
    private static final String OVERRIDE_FILE = "Download/droiddeck-driver";
    /** Bundled drivers the user deleted: hidden from the list, their unpacked copy removed. */
    private static final String PREFS = "graphics_driver";
    private static final String KEY_HIDDEN = "hiddenBundled";
    /** Stored choice meaning "pick by GPU" - the default, and what a removed import falls back to. */
    public static final String AUTO = "";

    public static final String HELP_TEXT =
            "The driver the app's compositor puts frames on the screen with - the last step of every "
            + "session, Steam or desktop. AdrenoTools zips only (vulkan.adXXXX.so on an Adreno, a bionic "
            + "PanVK on a Mali): a \"-Linux\" driver belongs in the Linux runtime list above and is "
            + "refused here. The compositor loads its "
            + "driver once per app process, so a change takes effect after the app is fully closed and "
            + "started again.";

    private final Context context;
    private final File contentDir;

    public TurnipDriver(Context context) {
        this.context = context;
        this.contentDir = new File(context.getFilesDir(), "graphics_driver");
    }

    /** Absolute directory holding the driver, with a trailing slash - adrenotools wants both. */
    public String driverPath(String driverId) {
        return new File(contentDir, driverId).getAbsolutePath() + "/";
    }

    public String libraryName(String driverId) {
        JSONObject meta = readMeta(driverId);
        return meta != null ? meta.optString("libraryName", null) : null;
    }

    /** meta.json's "name", or the id when it has none. */
    public String displayName(String driverId) {
        JSONObject meta = readMeta(driverId);
        String name = meta != null ? meta.optString("name", "") : "";
        return name.isEmpty() ? driverId : name;
    }

    public String driverVersion(String driverId) {
        JSONObject meta = readMeta(driverId);
        return meta != null ? meta.optString("driverVersion", "") : "";
    }

    private JSONObject readMeta(String driverId) {
        if (driverId == null || driverId.isEmpty() || driverId.contains("/") || driverId.contains("..")) return null;
        File meta = new File(new File(contentDir, driverId), "meta.json");
        String content = FileUtils.readString(meta);
        if (content == null) return null;
        try {
            return new JSONObject(content);
        } catch (Exception e) {
            Log.w(TAG, "meta.json for " + driverId, e);
            return null;
        }
    }

    /** An imported driver is installed when its meta.json names a library that is there. */
    public boolean isInstalled(String driverId) {
        String library = libraryName(driverId);
        return library != null && !library.isEmpty() && new File(new File(contentDir, driverId), library).isFile();
    }

    /**
     * The bundled drivers the list shows: {@link #BUNDLED} minus the ones the user deleted and the
     * ones this apk carries no archive for. A deleted one still lives in the apk and Auto still
     * unpacks it on a GPU that needs it, so deleting can never leave a device without a driver; it
     * only leaves the list.
     */
    public List<String> visibleBundled() {
        java.util.Set<String> hidden = hiddenBundled();
        ArrayList<String> out = new ArrayList<>();
        for (String id : BUNDLED) if (!hidden.contains(id) && carried(id)) out.add(id);
        return out;
    }

    /**
     * Whether this apk carries a bundled driver's archive. It is the only thing that decides if a
     * build can be offered: an id without an archive is never listed, never picked and never
     * unpacked, so a driver that would have to be built (the Mali PanVK) degrades to the phone's
     * system Vulkan driver - with the missing archive named here - rather than failing at session
     * start or being offered as a driver that is not there.
     */
    boolean carried(String driverId) {
        try (InputStream in = context.getAssets().open(ASSET_DIR + "adrenotools-" + driverId + ".tzst")) {
            return in != null;
        } catch (IOException e) {
            return false;
        }
    }

    /** A pick the apk carries no archive for is no pick at all: the blocker, said once, in the log. */
    private String carriedPick(String pick) {
        if (pick == null || carried(pick)) return pick;
        Log.w(TAG, "this apk carries no " + ASSET_DIR + "adrenotools-" + pick
                + ".tzst: no bundled driver for this GPU, so the compositor gets the system Vulkan driver");
        return null;
    }

    public java.util.Set<String> hiddenBundled() {
        return new java.util.HashSet<>(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getStringSet(KEY_HIDDEN, java.util.Collections.<String>emptySet()));
    }

    /** "Delete" for a bundled driver: remove its unpacked copy and hide it from the list. */
    public void hideBundled(String driverId) {
        if (!BUNDLED.contains(driverId)) return;
        FileUtils.delete(new File(contentDir, driverId));
        java.util.Set<String> hidden = hiddenBundled();
        hidden.add(driverId);
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putStringSet(KEY_HIDDEN, hidden).apply();
        Log.i(TAG, "bundled driver " + driverId + " hidden by the user");
    }

    public void restoreBundled() {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY_HIDDEN).apply();
    }

    /** Imported drivers only - the bundled two are listed by {@link #BUNDLED}. */
    public List<String> enumerateImported() {
        ArrayList<String> ids = new ArrayList<>();
        File[] dirs = contentDir.listFiles();
        if (dirs == null) return ids;
        for (File d : dirs) {
            String id = d.getName();
            if (d.isDirectory() && !BUNDLED.contains(id) && !id.startsWith(".") && isInstalled(id)) ids.add(id);
        }
        Collections.sort(ids);
        return ids;
    }

    /** The bundled build the GPU would pick on its own, for the "Auto" row's label. */
    public String autoId() {
        String id = chooseByDevice();
        return id == null ? "system" : id;
    }

    public void remove(String driverId) {
        if (driverId == null || driverId.isEmpty() || BUNDLED.contains(driverId)
                || driverId.contains("/") || driverId.contains("..")) return;
        Log.d(TAG, "removing imported driver " + driverId);
        FileUtils.delete(new File(contentDir, driverId));
        if (driverId.equals(SessionPrefs.androidDriver(context))) SessionPrefs.setAndroidDriver(context, AUTO);
    }

    /**
     * Import an AdrenoTools zip. Returns the new driver id.
     * @throws IllegalArgumentException with a user-facing reason when the zip is not an AdrenoTools
     *         driver - including when it is a Linux runtime driver, which says which list it belongs in;
     *         {@link IOException} on read/extract failures.
     */
    public String installFromZip(Uri zipUri, String displayName) throws IOException {
        File tmpDir = newStagingDir();
        boolean keep = false;
        try {
            try (InputStream is = context.getContentResolver().openInputStream(zipUri);
                 ZipInputStream zis = new ZipInputStream(is)) {
                if (is == null) throw new IOException("cannot open " + zipUri);
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    if (entry.isDirectory()) continue;
                    // Flatten: adrenotools reads the .so beside meta.json, and a base name also
                    // defeats zip-slip paths.
                    String base = new File(entry.getName()).getName();
                    if (base.isEmpty()) continue;
                    Files.copy(zis, new File(tmpDir, base).toPath(), StandardCopyOption.REPLACE_EXISTING);
                }
            }
            String id = adopt(tmpDir, displayName);
            keep = true;
            return id;
        } finally {
            if (!keep) FileUtils.delete(tmpDir);
        }
    }

    /** An empty directory beside the installed drivers, for a driver's files before {@link #adopt}. */
    File newStagingDir() throws IOException {
        if (!contentDir.exists()) contentDir.mkdirs();
        File tmpDir = new File(contentDir, ".tmp-" + System.nanoTime());
        FileUtils.delete(tmpDir);
        if (!tmpDir.mkdirs()) throw new IOException("cannot create " + tmpDir);
        return tmpDir;
    }

    /**
     * Check a staged driver (meta.json and its library, flat) and move it in as a new driver.
     * Returns the id; the caller deletes {@code tmpDir} when this throws.
     */
    String adopt(File tmpDir, String displayName) throws IOException {
        String rejected = rejectionReason(tmpDir);
        if (rejected != null) throw new IllegalArgumentException(context.getString(R.string.turnip_not_adrenotools, rejected));

        JSONObject meta = readMetaFile(new File(tmpDir, "meta.json"));
        String name = meta != null ? meta.optString("name", "") : "";
        if (name.isEmpty()) {
            name = displayName != null ? displayName : "adrenotools-driver";
            if (name.toLowerCase().endsWith(".zip")) name = name.substring(0, name.length() - 4);
        }
        String id = uniqueId(LinuxVulkanDriverManager.sanitizeId(name));
        File dir = new File(contentDir, id);
        if (!tmpDir.renameTo(dir)) throw new IOException("cannot move into " + dir);
        Log.i(TAG, "imported AdrenoTools driver " + id + " (" + (meta != null ? meta.optString("libraryName", "") : "?") + ") -> " + dir);
        return id;
    }

    private static JSONObject readMetaFile(File metaFile) {
        try {
            return new JSONObject(FileUtils.readString(metaFile));
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * Why a just-extracted zip is not something adrenotools can be pointed at, or null when it is.
     * The kinds refused by name are the other driver lists' zips, which carry a meta.json too.
     */
    private String rejectionReason(File dir) {
        File metaFile = new File(dir, "meta.json");
        if (!metaFile.isFile()) return context.getString(R.string.turnip_reject_no_meta);
        String libraryName, kind;
        try {
            JSONObject meta = new JSONObject(FileUtils.readString(metaFile));
            libraryName = meta.optString("libraryName", "");
            kind = meta.optString("kind", "");
        } catch (Exception e) {
            return context.getString(R.string.turnip_reject_meta_unreadable, e.getMessage());
        }
        if ("linux-vulkan-icd".equals(kind))
            return context.getString(R.string.turnip_reject_linux_driver);
        if ("wayland-game-driver".equals(kind))
            return context.getString(R.string.turnip_reject_wayland);
        if (libraryName.isEmpty())
            return context.getString(R.string.turnip_reject_no_library_name);
        File library = new File(dir, libraryName);
        if (!library.isFile())
            return context.getString(R.string.turnip_reject_library_missing, libraryName);
        if (!LinuxVulkanDriverManager.isAarch64Elf(library))
            return context.getString(R.string.turnip_reject_not_aarch64, libraryName);
        // A glibc build that happens to carry a libraryName would load nothing in this process.
        if (LinuxVulkanDriverManager.containsAscii(library, "libc.so.6"))
            return context.getString(R.string.turnip_reject_glibc, libraryName);
        return null;
    }

    private String uniqueId(String base) {
        String id = base;
        int n = 2;
        while (new File(contentDir, id).exists()) id = base + "-" + (n++);
        return id;
    }

    /**
     * Unpacks the driver this device needs and returns its id, or null when the app has no build for
     * this GPU - which hands the compositor the phone's own system Vulkan driver (a black session on
     * an Adreno, where the dma-buf import the compositor needs is missing) rather than a driver for a
     * GPU the phone does not have, and never an archive the apk does not carry.
     */
    public String install() {
        String id = choose();
        if (id == null) return null;
        File dir = new File(contentDir, id);
        if (!new File(dir, "meta.json").isFile()) {
            FileUtils.delete(dir);
            if (!TarZst.extractAsset(context, "graphics_driver/adrenotools-" + id + ".tzst", dir)) {
                Log.e(TAG, "could not unpack " + id);
                FileUtils.delete(dir);
                return null;
            }
        }
        String library = libraryName(id);
        if (library == null || !new File(dir, library).isFile()) {
            Log.e(TAG, id + " unpacked without " + library);
            return null;
        }
        Log.i(TAG, "graphics driver " + id + " (" + library + ")");
        return id;
    }

    /**
     * The user's choice first - an imported driver, or one of the bundled two pinned by hand -
     * then the Downloads override, then the GPU. A chosen import that has since been removed logs
     * and falls through, so a stale preference never blocks a session.
     */
    private String choose() {
        String chosen = SessionPrefs.androidDriver(context);
        if (chosen != null && !chosen.isEmpty()) {
            if (BUNDLED.contains(chosen)) {
                GpuInfo gpu = GpuInfo.detect();
                if (refusesPinned(gpu, chosen)) {
                    Log.w(TAG, "driver pinned by the user, " + chosen + ", is built for the other GPU vendor "
                            + "and this is " + gpu.getFamily().getVendor() + ": the compositor gets the system Vulkan driver");
                    return null;
                }
                Log.i(TAG, "driver pinned by the user: " + chosen);
                return carriedPick(bundledDriverFor(gpu, chosen, android.os.Build.VERSION.SDK_INT));
            }
            if (isInstalled(chosen)) {
                Log.i(TAG, "imported driver chosen by the user: " + chosen);
                return chosen;
            }
            Log.w(TAG, "chosen driver \"" + chosen + "\" is gone; picking by GPU");
        }
        return chooseByDevice();
    }

    private String chooseByDevice() {
        File override = new File(Environment.getExternalStorageDirectory(), OVERRIDE_FILE);
        String forced = override.isFile() ? FileUtils.readString(override) : null;
        String requested = null;
        if (forced != null) {
            forced = forced.trim().toLowerCase(java.util.Locale.US);
            Log.i(TAG, "driver forced by " + override + ": " + forced);
            if (forced.startsWith("system")) return null;
            if (forced.startsWith("a8")) requested = DRIVER_A8XX;
            else if (forced.startsWith("a7")) requested = DRIVER_A7XX;
            else if (forced.startsWith("panvk") || forced.startsWith("mali")) requested = DRIVER_MALI_G715;
        }
        GpuInfo gpu = GpuInfo.detect();
        Log.i(TAG, "gpu model: " + (gpu.getKgslName().isEmpty() ? "unknown" : gpu.getKgslName()));
        String pick = carriedPick(bundledDriverFor(gpu, requested, android.os.Build.VERSION.SDK_INT));
        if (pick == null) {
            Log.i(TAG, "no bundled driver applies to " + gpu.getFamily().getVendor() + "/" + gpu.getFamily().getLabel()
                    + ": the compositor gets the system Vulkan driver");
        }
        return pick;
    }

    /**
     * True when a bundled build the user pinned cannot run on this GPU: the pin names a build for the
     * other vendor - an Adreno Turnip on a Mali, or the Mali build on an Adreno - and would be handed
     * to a GPU it was not built for. The menu shows this reason; {@link #choose()} then picks nothing,
     * as {@link #bundledDriverFor} does for such a GPU. A pin that is not a bundled build at all (an
     * import, a missing one) is left alone.
     */
    public static boolean refusesPinned(GpuInfo gpu, String pinned) {
        if (pinned == null || !BUNDLED.contains(pinned)) return false;
        return buildVendor(pinned) != gpu.getFamily().getVendor();
    }

    /** True for the bundled builds built for Arm's kbase stack (a Mali), false for the Adreno Turnips. */
    public static boolean isMaliBuild(String driverId) {
        return DRIVER_MALI_G715.equals(driverId);
    }

    /** The vendor a bundled build was built for: Arm's kbase stack, or Qualcomm's KGSL. */
    private static GpuInfo.Vendor buildVendor(String driverId) {
        return isMaliBuild(driverId) ? GpuInfo.Vendor.MALI : GpuInfo.Vendor.ADRENO;
    }

    /**
     * The bundled driver this GPU needs, or null when the app has none to give it.
     *
     * <p>Which build works is a property of the device's graphics stack, not of its performance:
     * AdrenoTools loads a Turnip by hooking KGSL, so an Adreno needs one of the two Turnips; a Mali
     * (a Google Tensor phone) has no KGSL to hook and needs PanVK on kbase instead; and a GPU of
     * neither vendor gets nothing. Picking by generation alone - as this once did - handed
     * AdrenoTools a driver for a GPU the phone does not have. Null means "no driver of ours", which
     * {@link #install()} passes on as the phone's system Vulkan loader.
     *
     * <p>{@code requested} is a bundled id the user pinned or the Downloads override asked for, or
     * null. It is honoured only for the vendor it was built for: a Turnip asked for on a Mali, or the
     * Mali build asked for on an Adreno, is refused here too, so no path that names a bundled build
     * can hand a GPU a driver built for another stack.
     *
     * <p>Package-visible and free of Context, properties and file reads - the GPU is passed in as
     * {@link GpuInfo} - so the decision can be tested without a device.
     */
    static String bundledDriverFor(GpuInfo gpu, String requested, int sdk) {
        // Arm's stack: PanVK's own build for the generation, and only that. A Mali the app cannot
        // place gets nothing rather than a driver built for a generation it may not be.
        if (gpu.getFamily().getVendor() == GpuInfo.Vendor.MALI) {
            if (requested != null && buildVendor(requested) != GpuInfo.Vendor.MALI) return null;
            return gpu.getFamily() == GpuInfo.Family.MALI_G715 ? DRIVER_MALI_G715 : null;
        }
        if (gpu.getFamily().getVendor() != GpuInfo.Vendor.ADRENO) return null;
        if (requested != null && BUNDLED.contains(requested)) {
            return buildVendor(requested) == GpuInfo.Vendor.ADRENO ? requested : null;
        }
        String gpuModel = gpu.getKgslName();
        if (gpuModel != null && !gpuModel.isEmpty()) {
            // "Adreno750", "adreno_830" - the generation is the first digit of the three.
            java.util.regex.Matcher m = java.util.regex.Pattern.compile("(\\d)\\d\\d").matcher(gpuModel);
            if (m.find()) {
                char generation = m.group(1).charAt(0);
                if (generation >= '8') return DRIVER_A8XX;
                if (generation == '7') return DRIVER_A7XX;
            }
        }
        // Nothing to go on: Android 16 shipped with the 8 Elite, so treat a new device as 8xx.
        return sdk >= 36 ? DRIVER_A8XX : DRIVER_A7XX;
    }

}
