package com.droiddeck.launcher.gpu

import android.content.Context
import android.content.res.Configuration
import android.os.Build
import com.droiddeck.launcher.R
import com.droiddeck.launcher.core.FileUtils
import java.io.File
import java.util.Locale

/**
 * What GPU this is, in the terms the driver lists are sorted by. KGSL names the model
 * ("Adreno740v2", "Adreno825"); the family decides which Turnip builds run on it at all, and the
 * support level is what the app has actually been tested on (Adreno 650, 725 and newer) - an
 * Adreno 610 may start, but it is not "supported" just because it is a Qualcomm chip.
 */
data class GpuInfo(
    /** "Adreno 740", or the vendor's own name when this is not an Adreno. */
    val name: String,
    /** The three-digit Adreno model (740, 825), or 0 when KGSL does not say. */
    val model: Int,
    val family: Family,
    /** The SoC as a person would say it ("Snapdragon 8 Gen 2 (QCS8550)"), or "" when the phone does not say. */
    val soc: String,
    /** Samsung's One UI on an 8 Gen 2: its Turnip needs the OneUI build, or frames tear and flicker. */
    val oneUi8Gen2: Boolean,
    /** KGSL's own name for the GPU, as the kernel spells it ("Adreno740v2", "Adreno33v2"). */
    val kgslName: String = "",
    /** Where [model] came from: "kernel", "vulkan" or "platform", "" when unknown. */
    val modelSource: String = "",
) {
    /**
     * Which GPU this is, in the terms the driver lists are sorted by: the Adreno families are
     * Qualcomm's generations, the Mali ones are Arm's, and between them they decide which driver
     * builds can run at all. [NOT_ADRENO] is what is left - a GPU of neither vendor, which is where
     * this app has nothing to offer.
     */
    enum class Family(val label: String) {
        A8XX("Adreno 8xx"),
        /** Adreno 710/720/722: gen 7 cores cut down enough to need their own patches. */
        A7XX_LOW("Adreno 710/720/722"),
        A7XX("Adreno 7xx"),
        A6XX("Adreno 6xx"),
        /** An Adreno whose model KGSL does not give: treated as the newest family it could be. */
        ADRENO_UNKNOWN("Adreno"),
        /** Google Tensor G3 and G4 (Pixel 8, Pixel 9): Arm's Valhall v11, the Immortalis line. */
        MALI_G715("Mali-G715"),
        /** Google Tensor G1 and G2 (Pixel 6, Pixel 7): Valhall v9, the generation before the G715. */
        MALI_G78("Mali-G78"),
        /** A Mali the device does not name: kbase is there, the generation is not. */
        MALI_UNKNOWN("Mali"),
        /** Google Tensor G5 (Pixel 10): Imagination's D-Series, the GPU that is neither Adreno nor Mali. */
        POWERVR_GXX("PowerVR D-Series"),
        NOT_ADRENO("Not an Adreno GPU");

        /** Whose driver stack this family belongs to. */
        val vendor: Vendor
            get() = when (this) {
                A8XX, A7XX_LOW, A7XX, A6XX, ADRENO_UNKNOWN -> Vendor.ADRENO
                MALI_G715, MALI_G78, MALI_UNKNOWN -> Vendor.MALI
                POWERVR_GXX -> Vendor.POWERVR
                NOT_ADRENO -> Vendor.OTHER
            }

        /** [label] in the app's language: the Adreno families are names, the rest is words. */
        fun label(context: Context): String = if (this == NOT_ADRENO) context.getString(R.string.gpuinfo_not_adreno) else label
    }

    /** Whose graphics stack the device has: the two this app knows, and everything else. */
    enum class Vendor { ADRENO, MALI, POWERVR, OTHER }

    enum class Support { TESTED, UNTESTED, UNSUPPORTED }

    val support: Support
        get() = when {
            family == Family.NOT_ADRENO -> Support.UNSUPPORTED
            family == Family.A6XX && model == 650 -> Support.TESTED
            family == Family.A8XX -> Support.TESTED
            family == Family.A7XX && model >= 725 -> Support.TESTED
            else -> Support.UNTESTED
        }

    /** The card's line for this GPU, in the app's language. */
    fun supportText(context: Context): String = context.getString(when (support) {
        Support.TESTED -> R.string.gpuinfo_supported
        Support.UNTESTED -> when {
            family.vendor == Vendor.MALI -> R.string.gpuinfo_mali_experimental
            family.vendor == Vendor.POWERVR -> R.string.gpuinfo_powervr_experimental
            family == Family.A7XX_LOW -> R.string.gpuinfo_experimental
            else -> R.string.gpuinfo_below_tested
        }
        Support.UNSUPPORTED -> R.string.gpuinfo_unsupported
    })

    /**
     * [supportText] in English, for the device report and the device info. A Mali's line is the
     * resource's own English; an Adreno's keeps the wording those reports have always carried.
     */
    fun supportTextInEnglish(context: Context): String =
        if (family.vendor == Vendor.MALI || family.vendor == Vendor.POWERVR) {
        supportText(context.createConfigurationContext(Configuration(context.resources.configuration).apply { locale = Locale.ENGLISH }))
    } else when (support) {
        Support.TESTED -> "Supported"
        Support.UNTESTED -> if (family == Family.A7XX_LOW) "Experimental: its drivers are test builds"
            else "Outside tested hardware (Adreno 650, 725 and newer): it may not run"
        Support.UNSUPPORTED -> "Not supported: DroidDeck needs an Adreno (Snapdragon) GPU"
    }

    /** [name] for the screen: the stand-in for a GPU the device does not name is in the app's language. */
    fun displayName(context: Context): String = if (name == UNNAMED) context.getString(R.string.gpu_this_gpu) else name

    companion object {
        /** [name] when the device names no GPU; English, as the device report shows it. */
        private const val UNNAMED = "this GPU"

        /** The kernel's name for Qualcomm's GPU device, as both its sysfs directory and its /dev node use it. */
        const val KGSL_NAME = "kgsl-3d0"
        /** Qualcomm's kernel interface: the node only its stack has. */
        const val KGSL_NODE = "/dev/$KGSL_NAME"
        /** KGSL's sysfs directory for the same GPU. */
        const val KGSL_DIR = "/sys/class/kgsl/$KGSL_NAME"
        /** Qualcomm's Vulkan HAL, at the path every Adreno build puts it. */
        const val ADRENO_VULKAN_DRIVER = "/vendor/lib64/hw/vulkan.adreno.so"
        /** Arm's name for its GPU device, as the /dev node and the misc class both use it. */
        const val MALI_NAME = "mali0"
        /** Arm's kbase device: the node a Mali driver opens, the counterpart of KGSL's. */
        const val MALI_NODE = "/dev/$MALI_NAME"
        /** Where a kbase driver built as a misc device registers itself. */
        private const val MALI_CLASS = "/sys/class/misc/mali0"
        /** Arm's Vulkan HAL, as the Mali blob ships it. */
        private const val MALI_VULKAN_DRIVER = "/vendor/lib64/hw/vulkan.mali.so"
        /** Imagination's Vulkan HAL, at the path the PowerVR blob ships it. */
        private const val POWERVR_VULKAN_DRIVER = "/vendor/lib64/hw/vulkan.powervr.so"
        /** Where a PowerVR driver registers itself: PVRSync's misc device class. */
        private const val POWERVR_CLASS = "/sys/class/misc/pvr_sync"
        /** The kernel driver a Tensor G5's GPU runs on, as its render node's {@code uevent} names it. */
        const val POWERVR_DRIVER_NAME = "pvrsrvkm"
        /** The render node that driver exposes - the node a PowerVR driver opens. */
        const val POWERVR_RENDER_NODE = "/dev/dri/renderD128"

        /**
         * Adreno is recognised by what only Qualcomm's stack has: the KGSL node, or the vendor's own
         * Vulkan driver at its usual path. This is the one place that is decided - the driver pick and
         * the runtime's own checks ask for it here instead of repeating the paths.
         */
        @JvmStatic
        @JvmOverloads
        fun isAdreno(kgslDir: File = File(KGSL_DIR), vulkanDriver: File = File(ADRENO_VULKAN_DRIVER)): Boolean =
            kgslDir.exists() || vulkanDriver.exists()

        /** KGSL's name for the GPU as the kernel spells it, or null when the kernel does not say. */
        @JvmStatic
        fun kgslName(): String? = listOf("$KGSL_DIR/gpu_model", "$KGSL_DIR/gpu_chipid")
            .firstNotNullOfOrNull { FileUtils.readString(File(it))?.trim()?.takeIf(String::isNotEmpty) }

        /**
         * Mali is recognised the same way: Arm's kbase driver (its node, or the class a misc-device
         * build registers itself in), or Arm's Vulkan HAL. A Google Tensor phone has neither KGSL nor
         * an Adreno HAL, so on one of those this is what tells Mali from "a GPU we cannot name".
         */
        @JvmStatic
        @JvmOverloads
        fun isMali(
            kbaseDevice: File = File(MALI_NODE), kbaseClass: File = File(MALI_CLASS),
            vulkanDriver: File = File(MALI_VULKAN_DRIVER),
        ): Boolean = kbaseDevice.exists() || kbaseClass.exists() || vulkanDriver.exists()

        /**
         * PowerVR is recognised by its Vulkan HAL or its PVRSync device class. A Google Tensor phone
         * has neither KGSL nor a Mali HAL, so on one of those this is what tells PowerVR from
         * "a GPU we cannot name".
         */
        @JvmStatic
        @JvmOverloads
        fun isPowerVR(
            vulkanDriver: File = File(POWERVR_VULKAN_DRIVER),
            pvrSyncClass: File = File(POWERVR_CLASS),
        ): Boolean = vulkanDriver.exists() || pvrSyncClass.exists()

        /** The vendor of a GPU, from what was actually found. Adreno wins a phone that reports both. */
        internal fun vendorOf(adreno: Boolean, mali: Boolean, powervr: Boolean): Vendor = when {
            adreno -> Vendor.ADRENO
            mali -> Vendor.MALI
            powervr -> Vendor.POWERVR
            else -> Vendor.OTHER
        }

        @JvmStatic
        fun detect(): GpuInfo {
            val vendor = vendorOf(isAdreno(), isMali(), isPowerVR())
            val adreno = vendor == Vendor.ADRENO
            val mali = vendor == Vendor.MALI
            val powervr = vendor == Vendor.POWERVR
            val raw = kgslName()
            // Where vendors put the chip's model, named when it is known: "Snapdragon 8 Gen 2 (QCS8550)".
            // A Google Tensor phone answers here too, and on a Mali that answer is one of the two
            // things its generation is read from (the other is the GPU naming itself).
            val soc = if (vendor == Vendor.OTHER) "" else SocNames.label()
            val fromKernel = raw?.let { threeDigits(it) } ?: 0
            // Some kernels name the GPU without its model (AYANEO's Pocket FIT: "Adreno33v2"). The
            // Vulkan driver's own name, when this process has asked it, then the platform's code
            // name stand in - the GPU is the same on every phone of a platform.
            val fromVulkan = if (fromKernel > 0 || !(adreno || powervr)) 0 else VulkanInfo.cachedOrNull()?.get("device")?.let { threeDigits(it) } ?: 0
            val fromPlatform = if (fromKernel > 0 || fromVulkan > 0 || !(adreno || powervr)) 0 else platformModel()
            val model = maxOf(fromKernel, fromVulkan, fromPlatform)
            val source = when {
                fromKernel > 0 -> "kernel"
                fromVulkan > 0 -> "vulkan"
                fromPlatform > 0 -> "platform"
                else -> ""
            }
            val family = if (mali) maliFamily()
                else if (powervr) Family.POWERVR_GXX
                else familyOf(adreno, model)
            val samsung = Build.MANUFACTURER.equals("samsung", ignoreCase = true)
            return GpuInfo(
                name = when {
                    // The family's own label, which is how a driver list names it: "Mali-G715". A Mali
                    // is never named after Build.HARDWARE, because a Pixel's hardware is a platform
                    // code name ("zumapro") that says nothing to a person.
                    mali -> family.label
                    powervr -> family.label
                    !adreno -> Build.HARDWARE.ifBlank { UNNAMED }
                    model > 0 -> "Adreno $model"
                    else -> "Adreno"
                },
                model = model, family = family, soc = soc,
                oneUi8Gen2 = samsung && model == 740,
                kgslName = raw.orEmpty(), modelSource = source,
            )
        }

        private fun threeDigits(text: String): Int = Regex("""(\d{3})""").find(text)?.groupValues?.get(1)?.toIntOrNull() ?: 0

        /** Qualcomm platform code names whose GPU is known for certain. */
        private val PLATFORMS = mapOf(
            "msmnile" to 640, "kona" to 650, "lahaina" to 660, "taro" to 730, "cape" to 730,
            "kalama" to 740, "pineapple" to 750, "sun" to 830,
        )

        internal fun platformModel(platform: String = systemProperty("ro.board.platform")): Int =
            PLATFORMS[platform.lowercase()] ?: PLATFORMS[systemProperty("ro.vendor.qti.soc_name").lowercase()] ?: 0

        /** android.os.SystemProperties.get, which apps may read but not call directly. */
        fun systemProperty(name: String): String = try {
            Class.forName("android.os.SystemProperties").getMethod("get", String::class.java).invoke(null, name) as? String ?: ""
        } catch (_: Exception) {
            ""
        }

        /** A Google Tensor platform: the chip as a person says it, and the Mali its GPU is. */
        internal class Tensor(val chip: String, val family: Family)

        /**
         * Google's Tensor platforms, the one table that names them. Tensor G5 is here too: its GPU is
         * an Imagination PowerVR rather than an Arm Mali, and the table carries the family so the
         * naming and the vendor agree.
         */
        internal val TENSOR_PLATFORMS = mapOf(
            "gs101" to Tensor("Tensor G1", Family.MALI_G78),
            "gs201" to Tensor("Tensor G2", Family.MALI_G78),
            "zuma" to Tensor("Tensor G3", Family.MALI_G715),
            "zumapro" to Tensor("Tensor G4", Family.MALI_G715),
            "laguna" to Tensor("Tensor G5", Family.POWERVR_GXX),
        )

        /**
         * Mali generations named by their GPU's own name ("g715"), besides the Tensor platforms. Every GPU the
         * app has not been told about stays [Family.MALI_UNKNOWN] rather than being given a driver built for a
         * generation it is not.
         */
        private val MALI_GENERATIONS = mapOf(
            "g78" to Family.MALI_G78,
            "g715" to Family.MALI_G715,
        )

        /** The words of [text], lowercased: "Mali-G715 MC7" is read one token at a time. */
        private fun tokens(text: String): List<String> =
            Regex("[a-z0-9]+").findAll(text.lowercase()).map { it.value }.toList()

        /**
         * The Mali generation, from what the device says about itself - the GPU's own name first (the
         * Vulkan driver names itself "Mali-G715"), then the platform and model code names, whose Mali
         * generation is a matter of record. Nothing is guessed from the model number's digits: they do
         * not run in generation order (a G78 is older than a G715, but a G310 is newer than both).
         */
        internal fun maliFamilyOf(gpuName: String, socModel: String, hardware: String, platform: String): Family {
            for (text in listOf(gpuName, socModel, hardware, platform)) {
                for (token in tokens(text)) (MALI_GENERATIONS[token] ?: TENSOR_PLATFORMS[token]?.family)?.let { return it }
            }
            return Family.MALI_UNKNOWN
        }

        /** What this device says about its own Mali GPU, in the order the answers are trusted. */
        private fun maliFamily(): Family = maliFamilyOf(
            gpuName = VulkanInfo.cachedOrNull()?.get("device").orEmpty(),
            socModel = SocNames.model(),
            hardware = Build.HARDWARE,
            platform = systemProperty("ro.board.platform"),
        )

        internal fun familyOf(adreno: Boolean, model: Int): Family = when {
            !adreno -> Family.NOT_ADRENO
            model == 0 -> Family.ADRENO_UNKNOWN
            model >= 800 -> Family.A8XX
            model in listOf(710, 720, 722) -> Family.A7XX_LOW
            model >= 700 -> Family.A7XX
            model >= 600 -> Family.A6XX
            else -> Family.ADRENO_UNKNOWN
        }
    }
}
