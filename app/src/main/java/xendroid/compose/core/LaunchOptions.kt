package xendroid.compose.core

import java.io.File
import xendroid.compose.data.ProfileSlots
import xendroid.compose.settings.ConfigValueShape
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingsSchema

/**
 * R4 "Start with…": options for one launch only, never written to a config file. They reach the
 * core as command-line cvars (the host already passes --storage_root and --config this way), which
 * the core ranks above the game's and the global config. Pure, so the JVM tests hold the rules.
 *
 * The core exits on a launch option it cannot parse, so [sanitize] keeps only what [toArgs] makes:
 * a known setting with a value of its type, a profile slot with an XUID, the driver of an installed
 * package. The host also drops every option unless the launch carries this app's [LaunchToken].
 */
data class LaunchOptions(
    /** Who plays as P1 this time (XUID); null keeps the configured slots. */
    val profileXuid: String? = null,
    /** The Vulkan driver this time ("" = the system driver); null keeps the configured one. */
    val driverPath: String? = null,
    /** Another executable of the disc or package; null or blank keeps default.xex. */
    val launchModule: String? = null,
    /** Start without the game's patches. */
    val noPatches: Boolean = false,
    /** The game's own settings to replace by the global values this time (key -> global raw). */
    val ignoreGameSettings: Map<String, String> = emptyMap(),
    /** Extra arguments handed to the game (Kernel cl). */
    val extraCommandLine: String? = null,
) {
    val isEmpty: Boolean get() = toArgs(emptyList()).isEmpty()

    /** The command-line cvars, given the profile [slots] configured now (P1 first). */
    fun toArgs(slots: List<String?>): List<String> = buildList {
        for ((key, raw) in ignoreGameSettings.toSortedMap()) {
            val s = SettingsSchema.byKey[key] ?: continue
            if (s is Setting.Action && s.name != DRIVER) continue
            // The driver below wins over the game's own when both are asked for.
            if (s.name == DRIVER && driverPath != null) continue
            add("--${s.name}=${cliValue(s, raw)}")
        }
        profileXuid?.trim()?.uppercase()?.takeIf { XUID.matches(it) }?.let { xuid ->
            ProfileSlots.changes(slots, ProfileSlots.assign(slots, 0, xuid)).toSortedMap().forEach { (slot, value) ->
                add("--${ProfileSlots.key(slot)}=$value")
            }
        }
        driverPath?.let { add("--$DRIVER=$it") }
        launchModule?.trim()?.takeIf { it.isNotEmpty() && it.length <= MAX_VALUE }?.let { add("--launch_module=$it") }
        if (noPatches) add("--apply_patches=false")
        extraCommandLine?.trim()?.takeIf { it.isNotEmpty() && it.length <= MAX_VALUE }?.let { add("--cl=$it") }
    }

    companion object {
        const val DRIVER = "vulkan_lib_path"
        private const val MAX_VALUE = 1024
        private const val MAX_ARGS = 256
        private val XUID = Regex("[0-9A-F]{16}")
        private val ARG = Regex("^--([A-Za-z0-9_]+)=(.*)$", RegexOption.DOT_MATCHES_ALL)

        /** Never from a launch: where the core keeps its files and logs, and what it boots. */
        private val denied = setOf("storage_root", "config", "log_file", "log_append", "target", "cache_root", "content_root")

        private val byName: Map<String, Setting> by lazy {
            SettingsSchema.allSettings.filter { it !is Setting.Action || it.name == DRIVER }.associateBy { it.name }
        }
        private val slotKeys: Set<String> by lazy { (0 until ProfileSlots.COUNT).map { ProfileSlots.key(it) }.toSet() }

        /** [raw] as the core's option parser takes it for [s]. */
        fun cliValue(s: Setting, raw: String): String = when (s) {
            is Setting.Bool -> ConfigValueShape.bool(ConfigValueShape.parseBool(raw, s.default))
            is Setting.IntRange -> ConfigValueShape.parseInt(raw, s.default).coerceIn(s.min, s.max).toString()
            is Setting.ListChoice ->
                ConfigValueShape.listOption(s.options.map { it.value }, raw)?.takeIf { v -> s.options.any { it.value == v } } ?: s.default
            is Setting.Text -> raw
            is Setting.Action -> raw
        }

        /**
         * The arguments of [args] a launch may carry; the rest is dropped. [driverRoot] is where
         * driver packages are installed: a driver outside it (or no root) is never loaded.
         */
        fun sanitize(args: Array<String>?, driverRoot: File?): List<String> {
            val seen = HashSet<String>()
            return args.orEmpty().filter { arg ->
                if ('\u0000' in arg || '\n' in arg || '\r' in arg) return@filter false
                val m = ARG.find(arg) ?: return@filter false
                val name = m.groupValues[1]
                val value = m.groupValues[2]
                if (name in denied || value.length > MAX_VALUE || !seen.add(name)) return@filter false
                when {
                    name in slotKeys -> value.isEmpty() || XUID.matches(value)
                    name == DRIVER -> value.isEmpty() || isInstalledDriver(value, driverRoot)
                    else -> byName[name]?.let { valid(it, value) } ?: false
                }
            }.take(MAX_ARGS)
        }

        private fun valid(s: Setting, value: String): Boolean = when (s) {
            is Setting.Bool -> value == "true" || value == "false"
            is Setting.IntRange -> value.toIntOrNull()?.let { it in s.min..s.max } ?: false
            is Setting.ListChoice -> s.options.any { it.value == value }
            is Setting.Text -> true
            is Setting.Action -> false
        }

        /** A library file inside [root] (after resolving links and ".."). */
        fun isInstalledDriver(path: String, root: File?): Boolean {
            if (root == null || !path.endsWith(".so")) return false
            return runCatching {
                val file = File(path).canonicalFile
                val base = root.canonicalFile
                file.isFile && file.toPath().startsWith(base.toPath()) && file != base
            }.getOrDefault(false)
        }
    }
}
