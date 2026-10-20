package xendroid.compose.ui.settings

import android.content.Context
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.compositeOver
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import xendroid.compose.R
import xendroid.compose.core.SessionLogs
import xendroid.compose.settings.ConfigValueShape
import xendroid.compose.settings.GameSettingsViewModel
import xendroid.compose.settings.PinnedSettings
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingCatalog
import xendroid.compose.settings.SettingGroup
import xendroid.compose.settings.SettingLevel
import xendroid.compose.settings.SettingValue
import xendroid.compose.settings.SettingsHost
import xendroid.compose.settings.SettingsSchema
import xendroid.compose.settings.SettingsViewModel
import xendroid.compose.settings.Warning
import xendroid.compose.settings.setRaw
import xendroid.compose.ui.design.BadgeTone
import xendroid.compose.ui.design.LocalXdToast
import xendroid.compose.ui.design.NoteTone
import xendroid.compose.ui.design.Xd
import xendroid.compose.ui.design.XdBadge
import xendroid.compose.ui.design.XdButton
import xendroid.compose.ui.design.XdButtonKind
import xendroid.compose.ui.design.XdButtonSize
import xendroid.compose.ui.design.XdCard
import xendroid.compose.ui.design.XdChip
import xendroid.compose.ui.design.XdEmpty
import xendroid.compose.ui.design.XdGroupHeader
import xendroid.compose.ui.design.XdIcons
import xendroid.compose.ui.design.XdLink
import xendroid.compose.ui.design.XdListRow
import xendroid.compose.ui.design.XdNote
import xendroid.compose.ui.design.XdSearchField
import xendroid.compose.ui.design.XdSegmented
import xendroid.compose.ui.design.XdSelect
import xendroid.compose.ui.design.XdSlider
import xendroid.compose.ui.design.XdStepper
import xendroid.compose.ui.design.XdSwitch
import xendroid.compose.ui.design.XdText
import xendroid.compose.ui.design.XdTextInput
import xendroid.compose.ui.design.focusRing

// ------------------------------------------------------------------ data

/**
 * What a list of settings reads and edits: the global config, or one game's values on top of it.
 * The redesign's rows only read this, so the same rows serve Settings, the game sheet and tests.
 */
interface SettingsEditing {
    /** One game's values (false: the global config). */
    val forGame: Boolean
    val customDrivers: Boolean
    /** The value shown: the game's own or the inherited global one; the global config's. */
    fun raw(s: Setting): String
    /** The game has its own value; the global value is away from the default. */
    fun changed(s: Setting): Boolean
    /** The global value (what a game inherits). */
    fun globalRaw(s: Setting): String
    /** The value "back to default" (global) restores. */
    fun defaultRaw(s: Setting): String
    fun set(s: Setting, raw: String)
    /** Game: drop its own value. Global: back to the default. */
    fun reset(s: Setting)
    /** How many settings are changed (own values of the game; global ones away from default). */
    val changedCount: Int
    /** Global: the games with their own value of [s]. */
    fun gamesOverriding(s: Setting): List<String> = emptyList()
}

private fun schemaDefault(s: Setting): String = when (s) {
    is Setting.Bool -> ConfigValueShape.bool(s.default)
    is Setting.IntRange -> s.default.toString()
    is Setting.ListChoice -> s.default
    is Setting.Text -> s.default
    is Setting.Action -> s.default
}

/** The global config, through the Settings screen's view model. */
class GlobalSettingsEditing(
    private val vm: SettingsViewModel,
    private val values: Map<String, SettingValue>,
    private val overriding: Map<String, List<String>> = emptyMap(),
) : SettingsEditing {
    override val forGame = false
    override val customDrivers get() = vm.isCustomDriverSupported
    override fun raw(s: Setting) = values[s.key]?.raw ?: vm.defaultRaw(s)
    override fun changed(s: Setting) = values[s.key]?.modified == true
    override fun globalRaw(s: Setting) = raw(s)
    override fun defaultRaw(s: Setting) = vm.defaultRaw(s)
    override fun set(s: Setting, raw: String) = vm.setRaw(s, raw)
    override fun reset(s: Setting) = vm.resetToDefault(s)
    override val changedCount get() = values.values.count { it.modified }
    override fun gamesOverriding(s: Setting) = overriding[s.key].orEmpty()
}

/** One game's values over the global config, through its view model. */
class GameSettingsEditing(
    private val vm: GameSettingsViewModel,
    private val overrides: Map<String, String>,
) : SettingsEditing {
    override val forGame = true
    override val customDrivers get() = vm.isCustomDriverSupported
    override fun raw(s: Setting) = overrides[s.key] ?: vm.inheritedRaw(s)
    override fun changed(s: Setting) = s.key in overrides
    override fun globalRaw(s: Setting) = vm.inheritedRaw(s)
    override fun defaultRaw(s: Setting) = schemaDefault(s)
    override fun set(s: Setting, raw: String) {
        if (s !is Setting.Action && s.key !in overrides) vm.setOverride(s, true)
        vm.setRaw(s, raw)
    }
    override fun reset(s: Setting) = vm.setOverride(s, false)
    override val changedCount get() = overrides.size
}

/** A list in memory (previews and tests). */
class MapSettingsEditing(
    override val forGame: Boolean,
    private val global: MutableMap<String, String> = HashMap(),
    private val own: MutableMap<String, String> = HashMap(),
    override val customDrivers: Boolean = true,
    private val overriding: Map<String, List<String>> = emptyMap(),
    private val onChange: () -> Unit = {},
) : SettingsEditing {
    override fun raw(s: Setting) = (if (forGame) own[s.key] else null) ?: global[s.key] ?: schemaDefault(s)
    override fun changed(s: Setting) = if (forGame) s.key in own else (global[s.key] ?: schemaDefault(s)) != schemaDefault(s)
    override fun globalRaw(s: Setting) = global[s.key] ?: schemaDefault(s)
    override fun defaultRaw(s: Setting) = schemaDefault(s)
    override fun set(s: Setting, raw: String) { if (forGame) own[s.key] = raw else global[s.key] = raw; onChange() }
    override fun reset(s: Setting) { if (forGame) own.remove(s.key) else global.remove(s.key); onChange() }
    override val changedCount get() = if (forGame) own.size else SettingsSchema.allSettings.count { changed(it) }
    override fun gamesOverriding(s: Setting) = overriding[s.key].orEmpty()
}

/** The search, filter, level and text toggle of one settings screen. */
@Stable
class SettingsPanelState(level: SettingLevel) {
    var query by mutableStateOf("")
    var filter by mutableStateOf(SettingFilter.ALL)
    var level by mutableStateOf(level)
    var showDescriptions by mutableStateOf(true)
    /** On a game's screen: editing the global values instead of the game's. */
    var editGlobal by mutableStateOf(false)

    fun clear() { query = ""; filter = SettingFilter.ALL; level = SettingLevel.ALL }
}

enum class SettingFilter { ALL, CHANGED, NEW, LIVE, PINNED }

/** The rows [state] shows from [settings]; at [level] instead of the panel's (what the level switch counts). */
fun visibleSettings(
    settings: List<Setting>,
    state: SettingsPanelState,
    editing: SettingsEditing,
    pinned: List<String>,
    level: SettingLevel = state.level,
    searchText: (Setting) -> String,
): List<Setting> {
    val q = normalize(state.query.trim())
    return settings.filter { s ->
        val meta = SettingCatalog.meta(s)
        meta.level <= level &&
            (q.isEmpty() || normalize(searchText(s)).contains(q) || normalize(s.key.replace('|', '.')).contains(q)) &&
            when (state.filter) {
                SettingFilter.ALL -> true
                SettingFilter.CHANGED -> editing.changed(s)
                SettingFilter.NEW -> meta.isNew || meta.newOptions.isNotEmpty()
                SettingFilter.LIVE -> meta.live
                SettingFilter.PINNED -> s.key in pinned
            }
    }
}

/**
 * Round 2: what a settings panel lists in one tab ([group]; null: every group). [rows] are this
 * tab's at the panel's level; [perLevel], how many each level would list with the same search and
 * filter; [hidden], the matches of a search or filter that the level leaves out; [elsewhere], a
 * search's matches in the other tabs, at any level (the jump there says if the level hides them).
 */
class PanelResults(val rows: List<Setting>, val perLevel: List<Int>, val hidden: Int, val elsewhere: List<Setting>)

fun panelResults(
    group: SettingGroup?,
    state: SettingsPanelState,
    editing: SettingsEditing,
    pinned: List<String>,
    searchText: (Setting) -> String,
): PanelResults {
    val here = if (group == null) SettingCatalog.settings(SettingLevel.ALL) else SettingCatalog.settings(group)
    val perLevel = SettingLevel.entries.map { visibleSettings(here, state, editing, pinned, it, searchText) }
    val rows = perLevel[state.level.ordinal]
    val narrowed = state.query.isNotBlank() || state.filter != SettingFilter.ALL
    val hidden = if (narrowed) perLevel.last().size - rows.size else 0
    val elsewhere = if (state.query.isBlank() || group == null) emptyList()
    else SettingCatalog.groups.filter { it != group }
        .flatMap { visibleSettings(SettingCatalog.settings(it), state, editing, pinned, SettingLevel.ALL, searchText) }
    return PanelResults(rows, perLevel.map { it.size }, hidden, elsewhere)
}

private fun normalize(s: String): String =
    java.text.Normalizer.normalize(s, java.text.Normalizer.Form.NFD).replace(Regex("\\p{M}+"), "").lowercase()

// ------------------------------------------------------------------ texts

@Composable
fun groupTitle(group: SettingGroup): String = stringResource(when (group) {
    SettingGroup.IMAGE -> R.string.xd_group_image
    SettingGroup.PERFORMANCE -> R.string.xd_group_performance
    SettingGroup.AUDIO -> R.string.xd_group_audio
    SettingGroup.INPUT -> R.string.xd_group_input
    SettingGroup.COMPAT -> R.string.xd_group_compat
    SettingGroup.DRIVER -> R.string.xd_group_driver
    SettingGroup.SYSTEM -> R.string.xd_group_system
    SettingGroup.DEBUG -> R.string.xd_group_debug
})

fun groupIcon(group: SettingGroup) = when (group) {
    SettingGroup.IMAGE -> XdIcons.image
    SettingGroup.PERFORMANCE -> XdIcons.chart
    SettingGroup.AUDIO -> XdIcons.speaker
    SettingGroup.INPUT -> XdIcons.gamepad
    SettingGroup.COMPAT -> XdIcons.shield
    SettingGroup.DRIVER -> XdIcons.chip
    SettingGroup.SYSTEM -> XdIcons.cpu
    SettingGroup.DEBUG -> XdIcons.flask
}

@Composable
fun levelTitle(level: SettingLevel): String = stringResource(when (level) {
    SettingLevel.ESSENTIAL -> R.string.xd_set_level_essential
    SettingLevel.ADVANCED -> R.string.xd_set_level_advanced
    SettingLevel.ALL -> R.string.xd_set_level_all
})

private val UNITS = mapOf(
    "Display|present_safe_area_x" to "%", "Display|present_safe_area_y" to "%", "APU|volume" to "%",
    "Console|xmp_default_volume" to "%", "Vulkan|adrenotools_turbo_reassert_seconds" to " s",
    "GPU|texture_cache_memory_limit_soft" to " MB", "GPU|texture_cache_memory_limit_hard" to " MB",
    "Kernel|guest_scheduler_quantum_us" to " µs", "Video|internal_display_resolution_x" to " px",
    "Video|internal_display_resolution_y" to " px", "Kernel|stack_size_multiplier_hack" to "×",
)

/** A value as the rows show it: words translated, numbers with their unit. */
@Composable
fun valueLabel(s: Setting, raw: String): String {
    val context = LocalContext.current
    return valueLabel(context, s, raw)
}

fun valueLabel(context: Context, s: Setting, raw: String): String = when (s) {
    is Setting.Bool -> context.getString(if (ConfigValueShape.parseBool(raw, s.default)) R.string.xd_set_on else R.string.xd_set_off)
    is Setting.IntRange -> "${ConfigValueShape.parseInt(raw, s.default)}${UNITS[s.key].orEmpty()}"
    is Setting.ListChoice -> {
        val v = ConfigValueShape.listOption(s.options.map { it.value }, raw) ?: raw
        optionText(context, s, v, s.options.firstOrNull { it.value == v }?.label ?: v.ifEmpty { s.options.firstOrNull()?.label.orEmpty() })
    }
    is Setting.Text -> raw.ifEmpty { context.getString(R.string.xd_set_default_value, s.placeholder.ifEmpty { context.getString(R.string.xd_set_value_none) }) }
    is Setting.Action -> if (s.name == "vulkan_lib_path") driverName(context, raw) else raw
}

/** A driver path as a name: its package's (e.g. "Turnip v25.3.0"), its folder, or the system driver. */
fun driverName(context: Context, path: String): String =
    if (path.isBlank()) context.getString(R.string.drv_system)
    else xendroid.compose.driver.InstalledDrivers.nameOf(path, runCatching { xendroid.compose.Application.get_custom_driver_dir() }.getOrNull())
        ?: java.io.File(path).name

/** List options: words in the shown language; codes, sizes and filter names stay. */
fun optionText(context: Context, s: Setting.ListChoice, value: String, english: String): String {
    fun t(id: Int) = context.getString(id)
    return when (s.key) {
        "GPU|framerate_limit" -> if (value == "0") t(R.string.set_fps_unlimited) else english
        "GPU|anisotropic_override" -> when (value) { "-1" -> t(R.string.set_aniso_game); "0" -> t(R.string.set_option_off); else -> english }
        "HID|left_stick_deadzone_percentage", "HID|right_stick_deadzone_percentage" -> if (value == "0.0") t(R.string.set_option_off) else english
        "Display|postprocess_antialiasing" -> when (value) { "", "none" -> t(R.string.set_option_off); "fxaa" -> "FXAA"; "fxaa_extreme" -> t(R.string.xd_opt_fxaa_extreme); else -> english }
        "Display|postprocess_scaling_and_sharpening" -> when (value) {
            "", "bilinear" -> "Bilinear"; "cas" -> "AMD CAS"; "fsr" -> "FSR 1"; "sgsr" -> "SGSR (beta)"; "lanczos" -> "Lanczos"; "crt" -> "CRT"; else -> english
        }
        "Console|internal_display_resolution" -> if (value == "17") t(R.string.xd_opt_custom) else english
        "Kernel|kernel_display_gamma_type" -> when (value) { "0" -> "Linear"; "1" -> "sRGB (CRT)"; "2" -> "BT.709 (HDTV)"; "3" -> t(R.string.xd_opt_gamma_power); else -> english }
        "GPU|occlusion_query" -> when (value) {
            "fake" -> t(R.string.xd_opt_occ_fake); "fast" -> t(R.string.xd_opt_occ_fast)
            "fast-alt" -> t(R.string.xd_opt_occ_fast_alt); "strict" -> t(R.string.xd_opt_occ_strict); else -> english
        }
        "GPU|readback_resolve" -> when (value) {
            "uma" -> t(R.string.xd_opt_rb_uma); "fast" -> t(R.string.xd_opt_rb_fast); "all" -> t(R.string.xd_opt_rb_all)
            "none" -> t(R.string.set_option_off); else -> english
        }
        "Kernel|console_type" -> when (value) {
            "-1" -> t(R.string.xd_opt_console_retail); "0" -> t(R.string.xd_opt_console_devkit); "1" -> t(R.string.xd_opt_console_testkit); else -> english
        }
        "Logging|log_level" -> when (value) {
            "0" -> t(R.string.xd_opt_log_error); "1" -> t(R.string.xd_opt_log_warning); "2" -> t(R.string.xd_opt_log_info); "3" -> t(R.string.xd_opt_log_debug); else -> english
        }
        "Logging|log_mask" -> when (value) {
            "0" -> t(R.string.xd_opt_mask_all); "7" -> t(R.string.xd_opt_mask_gpu); "13" -> t(R.string.xd_opt_mask_audio)
            "14" -> t(R.string.xd_opt_mask_kernel); "11" -> t(R.string.xd_opt_mask_cpu); "1" -> t(R.string.xd_opt_mask_no_kernel)
            "8" -> t(R.string.xd_opt_mask_no_gpu); else -> english
        }
        "GPU|draw_resolution_scale_threshold" -> value.toIntOrNull()?.let { if (it == 0) t(R.string.set_option_off) else context.getString(R.string.xd_opt_up_to_px, it) } ?: english
        "Console|user_language" -> java.util.Locale(english).displayLanguage.replaceFirstChar { it.titlecase() }.ifBlank { english }
        "Console|user_country" -> java.util.Locale("", english).displayCountry.ifBlank { english }
        else -> english
    }
}

/** What the search matches besides the key: the shown title and description. */
fun settingSearchText2(context: Context, s: Setting): String = settingSearchText(context, s).ifBlank { s.title + " " + s.desc }

// ------------------------------------------------------------------ rows

/** The setting's dependency, when it does not apply now: "Only applies with X at Y." */
@Composable
private fun dependencyText(s: Setting, editing: SettingsEditing): String? {
    val dep = SettingCatalog.meta(s).dependsOn ?: return null
    if (SettingCatalog.applies(s) { key -> SettingsSchema.byKey[key]?.let { editing.raw(it) } }) return null
    val other = SettingsSchema.byKey[dep.key] ?: return null
    return stringResource(R.string.xd_set_depends, settingTitle(other), valueLabel(other, dep.value))
}

@Composable
private fun warningText(w: Warning): String = stringResource(when (w) {
    Warning.NEEDS_BARYCENTRICS -> R.string.xd_set_warn_barycentrics
    Warning.NO_EFFECT_ON_TURNIP -> R.string.xd_set_warn_turnip
    Warning.NEEDS_MAX_CLOCKS -> R.string.xd_set_warn_max_clocks
})

/**
 * One setting as a touch-mode row: title and badges; where the value comes from, when it applies,
 * the TOML key and "back to…"; the control and the pin; description, warning and why it does
 * not apply now.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun XdSettingRow(
    s: Setting,
    editing: SettingsEditing,
    showDescription: Boolean,
    pinned: Boolean,
    onPin: (() -> Unit)?,
    modifier: Modifier = Modifier,
    onOpenDrivers: (() -> Unit)? = null,
    onShowOverriding: ((Setting) -> Unit)? = null,
) {
    val c = Xd.colors
    val meta = SettingCatalog.meta(s)
    val changed = editing.changed(s)
    val dependency = dependencyText(s, editing)
    val shape = RoundedCornerShape(14.dp)
    BoxWithConstraints(modifier.fillMaxWidth()) {
        val narrow = maxWidth < 470.dp
        Column(
            Modifier.fillMaxWidth().clip(shape)
                .background(if (changed) c.acc.copy(alpha = 0.08f).compositeOver(c.solid(c.s1)) else c.s1)
                .then(if (changed) Modifier.border(1.dp, c.acc.copy(alpha = 0.22f), shape) else Modifier)
                .padding(start = 14.dp, end = 12.dp, top = 11.dp, bottom = 11.dp)
                .alpha(if (dependency != null) 0.62f else 1f),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Column(Modifier.weight(1f)) {
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(settingTitle(s), style = XdText.label, color = c.fg)
                        if (meta.isNew) XdBadge(stringResource(R.string.xd_set_badge_new), tone = BadgeTone.NEW, modifier = Modifier.align(Alignment.CenterVertically))
                        if (meta.newOptions.isNotEmpty()) XdBadge(stringResource(R.string.xd_set_badge_new_option), tone = BadgeTone.NEW, modifier = Modifier.align(Alignment.CenterVertically))
                    }
                    RowMeta(s, editing, changed, onShowOverriding)
                }
                if (!narrow) Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    SettingControl(s, editing, enabled = true, onOpenDrivers = onOpenDrivers)
                    if (onPin != null) PinButton(pinned, onPin)
                }
            }
            if (narrow) Row(Modifier.padding(top = 4.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                SettingControl(s, editing, enabled = true, onOpenDrivers = onOpenDrivers)
                if (onPin != null) PinButton(pinned, onPin)
            }
            if (showDescription) {
                val d = settingDesc(s)
                if (d.isNotBlank()) Text(d, style = XdText.note, color = c.fg3, modifier = Modifier.padding(top = 2.dp).widthIn(max = 640.dp))
            }
            meta.warning?.let { XdNote(warningText(it), tone = NoteTone.WARN, modifier = Modifier.padding(top = 2.dp)) }
            if (dependency != null) XdNote(dependency, tone = NoteTone.INFO, modifier = Modifier.padding(top = 2.dp))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RowMeta(s: Setting, editing: SettingsEditing, changed: Boolean, onShowOverriding: ((Setting) -> Unit)?) {
    val c = Xd.colors
    val meta = SettingCatalog.meta(s)
    FlowRow(Modifier.padding(top = 4.dp), horizontalArrangement = Arrangement.spacedBy(10.dp), verticalArrangement = Arrangement.spacedBy(5.dp)) {
        val tag = XdText.tiny.copy(fontWeight = FontWeight.SemiBold)
        val (srcText, srcBg, srcFg) = when {
            editing.forGame && changed -> Triple(stringResource(R.string.xd_set_src_game), c.acc.copy(alpha = 0.22f), c.acc)
            editing.forGame -> Triple(
                if (editing.globalRaw(s) != editing.defaultRaw(s)) stringResource(R.string.xd_set_src_global_changed) else stringResource(R.string.xd_set_src_global),
                c.s3, c.fg2)
            changed -> Triple(stringResource(R.string.xd_set_src_changed), c.changed.copy(alpha = 0.2f), c.changed)
            else -> Triple(stringResource(R.string.xd_set_src_default), c.s3, c.fg2)
        }
        Text(srcText, style = tag, color = srcFg, modifier = Modifier.align(Alignment.CenterVertically).clip(RoundedCornerShape(6.dp))
            .background(srcBg).padding(horizontal = 7.dp, vertical = 2.dp))
        if (!editing.forGame) {
            val n = editing.gamesOverriding(s).size
            if (n > 0) MetaButton(pluralStringResource(R.plurals.xd_set_games_override, n, n)) { onShowOverriding?.invoke(s) }
        }
        val applyIcon = if (meta.live) XdIcons.bolt else XdIcons.restart
        Row(Modifier.align(Alignment.CenterVertically), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
            Icon(applyIcon, null, Modifier.size(12.dp), tint = if (meta.live) c.acc else c.fg3)
            Text(stringResource(if (s is Setting.Action && s.name == "dump_session_logs") R.string.xd_set_action
                else if (meta.live) R.string.xd_set_live else R.string.xd_set_next_launch), style = tag, color = if (meta.live) c.acc else c.fg3)
        }
        if (s !is Setting.Action || s.name == "vulkan_lib_path") {
            Text(s.key.replace('|', '.'), style = XdText.monoSm, color = c.fg3.copy(alpha = 0.85f), modifier = Modifier.align(Alignment.CenterVertically))
        }
        if (changed && s.name != "dump_session_logs") {
            val label = if (editing.forGame) stringResource(R.string.xd_set_reset_global, valueLabel(s, editing.globalRaw(s)))
            else stringResource(R.string.xd_set_reset_default)
            MetaButton(label, icon = true) { editing.reset(s) }
        }
    }
}

@Composable
private fun MetaButton(text: String, icon: Boolean = false, onClick: () -> Unit) {
    val c = Xd.colors
    val shape = RoundedCornerShape(6.dp)
    Row(
        Modifier.focusRing(shape).clip(shape).background(c.s2).clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = 6.dp, vertical = 2.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp),
    ) {
        if (icon) Icon(XdIcons.reset, null, Modifier.size(12.dp), tint = c.fg2)
        Text(text, style = XdText.tiny.copy(fontWeight = FontWeight.SemiBold), color = c.fg2)
    }
}

@Composable
private fun PinButton(pinned: Boolean, onClick: () -> Unit) {
    val c = Xd.colors
    val shape = RoundedCornerShape(8.dp)
    Box(
        Modifier.focusRing(shape).size(30.dp).clip(shape).clickable(role = Role.Checkbox, onClick = onClick),
        contentAlignment = Alignment.Center,
    ) { Icon(XdIcons.pin, stringResource(R.string.xd_set_pin), Modifier.size(16.dp), tint = if (pinned) c.acc else c.fg3) }
}

/** The control a setting edits with, by its kind (switch, segments or list, stepper or slider…). */
@Composable
fun SettingControl(s: Setting, editing: SettingsEditing, enabled: Boolean, compact: Boolean = false, onOpenDrivers: (() -> Unit)? = null) {
    val raw = editing.raw(s)
    when (s) {
        is Setting.Bool -> XdSwitch(ConfigValueShape.parseBool(raw, s.default), { editing.set(s, ConfigValueShape.bool(it)) },
            enabled = enabled, contentDescription = settingTitle(s))
        is Setting.ListChoice -> {
            val current = ConfigValueShape.listOption(s.options.map { it.value }, raw) ?: raw
            val labels = s.options.map { it.value to valueLabel(s, it.value) }
            val total = labels.sumOf { it.second.length }
            val segments = if (compact) s.options.size <= 3 && total <= 12 else s.options.size <= 6 && total <= 26
            if (segments) XdSegmented(labels, current, { editing.set(s, it) }, enabled = enabled, compact = compact)
            else XdSelect(labels, current, { editing.set(s, it) }, enabled = enabled, maxWidth = if (compact) 150.dp else 220.dp,
                placeholder = valueLabel(s, raw))
        }
        is Setting.IntRange -> {
            val v = ConfigValueShape.parseInt(raw, s.default)
            if (s.max - s.min <= 10) {
                XdStepper(valueLabel(s, raw), { editing.set(s, (v - 1).coerceAtLeast(s.min).toString()) },
                    { editing.set(s, (v + 1).coerceAtMost(s.max).toString()) }, enabled = enabled, canPrevious = v > s.min, canNext = v < s.max)
            } else {
                var drag by remember(s.key, v) { mutableStateOf(v.toFloat()) }
                val step = stepOf(s)
                XdSlider(drag, { drag = it }, s.min.toFloat()..s.max.toFloat(), steps = 0,
                    label = valueLabel(s, drag.toInt().toString()), enabled = enabled,
                    onValueChangeFinished = { editing.set(s, ((Math.round(drag / step) * step).toInt()).coerceIn(s.min, s.max).toString()) })
            }
        }
        is Setting.Text -> {
            var text by remember(s.key, raw) { mutableStateOf(raw) }
            XdTextInput(text, { text = it; editing.set(s, it) }, placeholder = s.placeholder, enabled = enabled)
        }
        is Setting.Action -> when (s.name) {
            "dump_session_logs" -> ExportLogsButton()
            else -> {
                var picking by remember { mutableStateOf(false) }
                XdButton(valueLabel(s, raw), { picking = true }, size = XdButtonSize.SM, icon = XdIcons.chip,
                    enabled = enabled && editing.customDrivers)
                if (picking) DriverPickerSheet(s, editing, onOpenDrivers) { picking = false }
            }
        }
    }
}

private fun stepOf(s: Setting.IntRange): Float = when (s.key) {
    "Video|internal_display_resolution_x" -> 16f
    "Video|internal_display_resolution_y" -> 8f
    "GPU|vulkan_mid_frame_submission_draws" -> 50f
    "GPU|texture_cache_memory_limit_soft", "GPU|texture_cache_memory_limit_hard" -> 64f
    "Kernel|guest_scheduler_quantum_us" -> 250f
    "APU|volume", "Console|xmp_default_volume" -> 5f
    else -> 1f
}

@Composable
internal fun ExportLogsButton() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var busy by remember { mutableStateOf(false) }
    XdButton(stringResource(if (busy) R.string.set_logs_exporting else R.string.xd_set_export), {
        busy = true
        scope.launch {
            val dest = withContext(Dispatchers.IO) { runCatching { SessionLogs.exportAll() }.getOrNull() }
            Toast.makeText(context, dest?.let { context.getString(R.string.set_logs_exported, it.name) }
                ?: context.getString(R.string.set_logs_none), Toast.LENGTH_LONG).show()
            busy = false
        }
    }, size = XdButtonSize.SM, icon = XdIcons.download, enabled = !busy)
}

/**
 * One setting as a controller-mode row: title and where it comes from on the left, ◀ value ▶
 * on the right; left/right change it, A toggles or goes to the next value.
 */
@Composable
fun XdControllerSettingRow(s: Setting, editing: SettingsEditing, modifier: Modifier = Modifier, onOpenDrivers: (() -> Unit)? = null) {
    val c = Xd.colors
    val context = LocalContext.current
    val meta = SettingCatalog.meta(s)
    val changed = editing.changed(s)
    val dependency = dependencyText(s, editing)
    val raw = editing.raw(s)
    var focused by remember { mutableStateOf(false) }
    var picking by remember { mutableStateOf(false) }
    if (picking) DriverPickerSheet(s, editing, onOpenDrivers) { picking = false }
    val shape = RoundedCornerShape(13.dp)
    val sub = when {
        dependency != null -> dependency
        editing.forGame && changed -> stringResource(R.string.xd_set_c_game_from_global, valueLabel(s, editing.globalRaw(s)))
        editing.forGame -> if (meta.live) stringResource(R.string.xd_set_c_live_global) else stringResource(R.string.xd_set_src_global)
        else -> (if (changed) stringResource(R.string.xd_set_src_changed) else stringResource(R.string.xd_set_src_default))
            .let { if (meta.live) stringResource(R.string.xd_set_c_live, it) else it }
    }
    fun step(dir: Int, wrap: Boolean) {
        val next = stepped(s, raw, dir, wrap) ?: return
        editing.set(s, next)
    }
    Row(
        modifier.fillMaxWidth().heightIn(min = 54.dp)
            .onFocusChanged { focused = it.isFocused }
            .clip(shape)
            .background(when {
                focused -> c.acc.copy(alpha = 0.22f).compositeOver(Color.Black.copy(alpha = 0.2f))
                changed -> c.acc.copy(alpha = 0.13f).compositeOver(Color.White.copy(alpha = 0.05f))
                else -> c.s1
            })
            .then(if (focused) Modifier.border(2.dp, c.acc, shape) else Modifier)
            .onPreviewKeyEvent { e ->
                if (e.type != KeyEventType.KeyDown || s is Setting.Text) return@onPreviewKeyEvent false
                when (e.key) {
                    Key.DirectionLeft -> { if (dependency == null) step(-1, false); true }
                    Key.DirectionRight -> { if (dependency == null) step(1, false); true }
                    else -> false
                }
            }
            .clickable(role = Role.Button) {
                when {
                    dependency != null -> Toast.makeText(context, dependency, Toast.LENGTH_SHORT).show()
                    s is Setting.Action && s.name == "vulkan_lib_path" -> if (editing.customDrivers) picking = true
                    else -> step(1, true)
                }
            }
            .alpha(if (dependency != null) 0.55f else 1f)
            .padding(start = 16.dp, end = 10.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(settingTitle(s), style = XdText.label.copy(fontSize = 14.5.sp), color = c.fg, modifier = Modifier.weight(1f, fill = false))
                if (meta.isNew) XdBadge(stringResource(R.string.xd_set_badge_new), tone = BadgeTone.NEW)
            }
            Text(sub, style = XdText.tiny, color = c.fg3, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.padding(top = 3.dp))
        }
        when (s) {
            is Setting.Bool -> XdSwitch(ConfigValueShape.parseBool(raw, s.default), null)
            is Setting.Text -> XdTextInput(raw, { editing.set(s, it) }, placeholder = s.placeholder, width = 220.dp)
            is Setting.Action -> if (s.name == "dump_session_logs") ExportLogsButton()
                else Text(valueLabel(s, raw), style = XdText.label, color = c.fg)
            else -> Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                ArrowBox(XdIcons.chevL)
                Text(valueLabel(s, raw), style = XdText.label, color = c.fg, maxLines = 1, overflow = TextOverflow.Ellipsis,
                    textAlign = androidx.compose.ui.text.style.TextAlign.Center, modifier = Modifier.widthIn(min = 84.dp, max = 170.dp))
                ArrowBox(XdIcons.chevR)
            }
        }
    }
}

@Composable
private fun ArrowBox(icon: androidx.compose.ui.graphics.vector.ImageVector) {
    val c = Xd.colors
    Box(Modifier.size(30.dp).clip(RoundedCornerShape(9.dp)).background(c.s2), contentAlignment = Alignment.Center) {
        Icon(icon, null, Modifier.size(16.dp), tint = c.fg2)
    }
}

/** The next value of [s] from [raw] in [dir]; [wrap] goes round at the ends (A on a list). */
fun stepped(s: Setting, raw: String, dir: Int, wrap: Boolean): String? = when (s) {
    is Setting.Bool -> ConfigValueShape.bool(!ConfigValueShape.parseBool(raw, s.default))
    is Setting.ListChoice -> {
        val values = s.options.map { it.value }
        val i = values.indexOf(ConfigValueShape.listOption(values, raw)).let { if (it < 0) 0 else it }
        val n = i + dir
        values[if (wrap) Math.floorMod(n, values.size) else n.coerceIn(0, values.lastIndex)]
    }
    is Setting.IntRange -> {
        val step = stepOf(s).toInt().coerceAtLeast(1)
        var n = ConfigValueShape.parseInt(raw, s.default) + dir * step
        if (wrap && n > s.max) n = s.min
        n.coerceIn(s.min, s.max).toString()
    }
    else -> null
}

/** A compact row for the quick settings: title (a dot when the game changes it) and the control. */
@Composable
fun XdQuickRow(s: Setting, editing: SettingsEditing, modifier: Modifier = Modifier, onOpenDrivers: (() -> Unit)? = null) {
    val c = Xd.colors
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().heightIn(min = 42.dp).padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(settingTitle(s), style = XdText.labelSm, color = c.fg2, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                if (editing.forGame && editing.changed(s)) Box(Modifier.size(6.dp).clip(RoundedCornerShape(50)).background(c.acc))
            }
            SettingControl(s, editing, enabled = true, compact = true, onOpenDrivers = onOpenDrivers)
        }
        HorizontalDivider(thickness = 1.dp, color = c.line)
    }
}

/** The resolution scale as one control (width and height together); "Mixed" when they differ. */
@Composable
fun XdResolutionRow(editing: SettingsEditing, compact: Boolean, modifier: Modifier = Modifier) {
    val c = Xd.colors
    val x = SettingsSchema.byKey["GPU|draw_resolution_scale_x"] as Setting.ListChoice
    val y = SettingsSchema.byKey["GPU|draw_resolution_scale_y"] as Setting.ListChoice
    val vx = editing.raw(x)
    val vy = editing.raw(y)
    val options = x.options.map { it.value to it.label }
    val set: (String) -> Unit = { editing.set(x, it); editing.set(y, it) }
    if (Xd.controller) {
        val shape = RoundedCornerShape(13.dp)
        var focused by remember { mutableStateOf(false) }
        Row(
            modifier.fillMaxWidth().heightIn(min = 54.dp).onFocusChanged { focused = it.isFocused }.clip(shape)
                .background(if (focused) c.acc.copy(alpha = 0.22f) else c.s1)
                .then(if (focused) Modifier.border(2.dp, c.acc, shape) else Modifier)
                .onPreviewKeyEvent { e ->
                    if (e.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
                    val d = when (e.key) { Key.DirectionLeft -> -1; Key.DirectionRight -> 1; else -> 0 }
                    if (d == 0) false else { stepped(x, vx, d, false)?.let(set); true }
                }
                .clickable { stepped(x, vx, 1, true)?.let(set) }
                .padding(start = 16.dp, end = 10.dp, top = 8.dp, bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.xd_set_resolution), style = XdText.label.copy(fontSize = 14.5.sp), color = c.fg)
                Text(stringResource(R.string.xd_set_resolution_desc), style = XdText.tiny, color = c.fg3)
            }
            ArrowBox(XdIcons.chevL)
            Text(if (vx == vy) "${vx}x" else stringResource(R.string.xd_set_mixed), style = XdText.label, color = c.fg,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center, modifier = Modifier.widthIn(min = 84.dp))
            ArrowBox(XdIcons.chevR)
        }
        return
    }
    Column(modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth().heightIn(min = 42.dp).padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Text(stringResource(R.string.xd_set_resolution), style = XdText.labelSm, color = c.fg2)
                if (editing.forGame && (editing.changed(x) || editing.changed(y))) Box(Modifier.size(6.dp).clip(RoundedCornerShape(50)).background(c.acc))
            }
            XdSegmented(options, if (vx == vy) vx else null, set, compact = compact)
        }
        HorizontalDivider(thickness = 1.dp, color = c.line)
    }
}

// ------------------------------------------------------------------ the panel

/**
 * Search, level and filters over a list of settings, then the rows: by group (with headers) or
 * one [group]. On a game's screen, the scope switch edits the game's values or the global ones.
 *
 * Round 2: the search looks in this tab only; what matches in the other tabs shows under "In
 * other tabs", and [onJump] goes there (the search stays, so the setting is in view). Each level
 * says how many settings it would list here, and a search or filter says how many results the
 * level hides, with "Show". Without [header] (the Summary's own search), only the results show.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun XdSettingsPanel(
    editing: SettingsEditing,
    state: SettingsPanelState,
    pinned: List<String>,
    onPinnedChange: (List<String>) -> Unit,
    modifier: Modifier = Modifier,
    group: SettingGroup? = null,
    gameName: String? = null,
    onPresets: (() -> Unit)? = null,
    onToml: (() -> Unit)? = null,
    onResetAll: (() -> Unit)? = null,
    onOpenDrivers: (() -> Unit)? = null,
    onShowOverriding: ((Setting) -> Unit)? = null,
    scopeSwitch: (@Composable () -> Unit)? = null,
    onJump: ((SettingGroup) -> Unit)? = null,
    header: Boolean = true,
) {
    val c = Xd.colors
    val context = LocalContext.current
    val found = panelResults(group, state, editing, pinned) { settingSearchText2(context, it) }
    val rows = found.rows
    val hidden = found.hidden
    val elsewhere = found.elsewhere
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (header) PanelHeader(editing, state, pinned, group, found.perLevel, gameName, onPresets, onToml, onResetAll, scopeSwitch)
        if (hidden > 0) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(pluralStringResource(R.plurals.xd_set_hidden_by_level, hidden, hidden, levelTitle(state.level)), style = XdText.note, color = c.fg3)
            XdLink(stringResource(R.string.xd_set_show_hidden), { state.level = SettingLevel.ALL })
        }
        when {
            rows.isNotEmpty() -> SettingRows(rows, editing, state, pinned, onPinnedChange, byGroup = group == null,
                onOpenDrivers = onOpenDrivers, onShowOverriding = onShowOverriding, onJump = onJump)
            elsewhere.isEmpty() && hidden == 0 ->
                XdEmpty(stringResource(R.string.xd_set_none)) { XdLink(stringResource(R.string.xd_set_clear_filters), { state.clear() }) }
            group != null -> Text(stringResource(R.string.xd_set_none_in, groupTitle(group)), style = XdText.bodySm, color = c.fg2)
        }
        if (elsewhere.isNotEmpty()) OtherTabs(elsewhere, onJump)
    }
}

/** The panel's search, level (each saying how many it lists here), filters, and what is edited. */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun PanelHeader(
    editing: SettingsEditing,
    state: SettingsPanelState,
    pinned: List<String>,
    group: SettingGroup?,
    perLevel: List<Int>,
    gameName: String?,
    onPresets: (() -> Unit)?,
    onToml: (() -> Unit)?,
    onResetAll: (() -> Unit)?,
    scopeSwitch: (@Composable () -> Unit)?,
) {
    val c = Xd.colors
    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        scopeSwitch?.invoke()
        XdSearchField(state.query, { state.query = it },
            if (group != null) stringResource(R.string.xd_set_search_in, groupTitle(group)) else stringResource(R.string.xd_set_search),
            Modifier.widthIn(min = 220.dp, max = 420.dp))
        XdSegmented(SettingLevel.entries.map { it to levelTitle(it) }, state.level, { state.level = it }, counts = perLevel)
    }
    FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        val here = remember(group) { if (group == null) SettingCatalog.settings(SettingLevel.ALL) else SettingCatalog.settings(group) }
        val counted = { f: SettingFilter ->
            visibleSettings(here, SettingsPanelState(state.level).also { it.filter = f }, editing, pinned) { "" }.size
        }
        listOf(
            SettingFilter.ALL to stringResource(R.string.xd_set_filter_all),
            SettingFilter.CHANGED to stringResource(if (editing.forGame) R.string.xd_set_filter_changed_game else R.string.xd_set_filter_changed),
            SettingFilter.NEW to stringResource(R.string.xd_set_filter_new),
            SettingFilter.LIVE to stringResource(R.string.xd_set_filter_live),
            SettingFilter.PINNED to stringResource(R.string.xd_set_filter_pinned),
        ).forEach { (f, label) ->
            XdChip(label, state.filter == f, { state.filter = f }, count = if (f == SettingFilter.ALL) null else counted(f))
        }
        if (onPresets != null) XdButton(stringResource(R.string.xd_set_presets), onPresets, size = XdButtonSize.SM, icon = XdIcons.wand)
        if (onToml != null) XdButton(stringResource(if (editing.forGame) R.string.xd_set_toml else R.string.xd_set_toml_global), onToml,
            size = XdButtonSize.SM, icon = XdIcons.code)
        XdButton(stringResource(if (state.showDescriptions) R.string.xd_set_less_text else R.string.xd_set_more_text),
            { state.showDescriptions = !state.showDescriptions }, size = XdButtonSize.SM, kind = XdButtonKind.GHOST, icon = XdIcons.info)
    }
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Icon(XdIcons.info, null, Modifier.padding(top = 1.dp).size(15.dp), tint = c.fg3)
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                when {
                    !editing.forGame && gameName != null -> stringResource(R.string.xd_set_note_game_global)
                    !editing.forGame -> stringResource(R.string.xd_set_note_global, editing.changedCount)
                    else -> stringResource(R.string.xd_set_note_game, gameName.orEmpty())
                },
                style = XdText.note, color = c.fg3,
            )
            if (editing.forGame && editing.changedCount > 0 && onResetAll != null) XdLink(stringResource(R.string.xd_set_reset_all), onResetAll)
        }
    }
}

/** The rows: one list in a tab, or under each group's header ("Open the tab" when [onJump] goes there). */
@Composable
private fun SettingRows(
    rows: List<Setting>,
    editing: SettingsEditing,
    state: SettingsPanelState,
    pinned: List<String>,
    onPinnedChange: (List<String>) -> Unit,
    byGroup: Boolean,
    onOpenDrivers: (() -> Unit)?,
    onShowOverriding: ((Setting) -> Unit)?,
    onJump: ((SettingGroup) -> Unit)?,
) {
    val c = Xd.colors
    val context = LocalContext.current
    val toast = LocalXdToast.current
    val pin: (Setting) -> (() -> Unit)? = { s ->
        if (s is Setting.Action) null else ({
            val next = PinnedSettings.toggled(pinned, s.key)
            onPinnedChange(next)
            toast.show(context.getString(if (s.key in next) R.string.xd_set_pinned else R.string.xd_set_unpinned))
        })
    }
    val one: @Composable (Setting) -> Unit = { s ->
        if (c.controller) XdControllerSettingRow(s, editing, onOpenDrivers = onOpenDrivers)
        else XdSettingRow(s, editing, state.showDescriptions, s.key in pinned, pin(s), onOpenDrivers = onOpenDrivers, onShowOverriding = onShowOverriding)
    }
    if (!byGroup) {
        Column(verticalArrangement = Arrangement.spacedBy(6.dp)) { rows.forEach { one(it) } }
        return
    }
    Column {
        for (g in SettingCatalog.groups) {
            val inGroup = rows.filter { SettingCatalog.meta(it).group == g }
            if (inGroup.isEmpty()) continue
            Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                XdGroupHeader(groupTitle(g), inGroup.size, Modifier.weight(1f))
                if (onJump != null) XdLink(stringResource(R.string.xd_set_open_tab) + " ›", { onJump(g) }, Modifier.padding(bottom = 6.dp))
            }
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) { inGroup.forEach { one(it) } }
        }
    }
}

/** "In other tabs": up to five matches of the search elsewhere, each with its tab; a tap goes there. */
@Composable
private fun OtherTabs(found: List<Setting>, onJump: ((SettingGroup) -> Unit)?) {
    val c = Xd.colors
    val shown = found.take(5)
    Column {
        XdGroupHeader(stringResource(R.string.xd_set_other_tabs), found.size)
        XdCard(tight = true) {
            shown.forEachIndexed { i, s ->
                val g = SettingCatalog.meta(s).group
                XdListRow(settingTitle(s), subtitle = groupTitle(g), icon = groupIcon(g), divider = i < shown.lastIndex,
                    onClick = onJump?.let { jump -> { jump(g) } }) {
                    if (onJump != null) Icon(XdIcons.chevR, null, Modifier.size(16.dp), tint = c.fg3)
                }
            }
            if (found.size > shown.size) {
                val more = found.size - shown.size
                Text(pluralStringResource(R.plurals.xd_set_other_more, more, more), style = XdText.note, color = c.fg3,
                    modifier = Modifier.padding(start = 4.dp, top = 6.dp))
            }
        }
    }
}

/** The quick settings: the pinned ones, compact (touch) or as controller rows. */
@Composable
fun XdQuickSettings(editing: SettingsEditing, pinned: List<String>, modifier: Modifier = Modifier, onOpenDrivers: (() -> Unit)? = null) {
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(if (Xd.controller) 6.dp else 0.dp)) {
        for (key in pinned) {
            if (key == PinnedSettings.RESOLUTION) { XdResolutionRow(editing, compact = true); continue }
            val s = SettingsSchema.byKey[key] ?: continue
            if (Xd.controller) XdControllerSettingRow(s, editing, onOpenDrivers = onOpenDrivers)
            else XdQuickRow(s, editing, onOpenDrivers = onOpenDrivers)
        }
    }
}

// ------------------------------------------------------------------ TOML and presets

/** The TOML a game's file holds (only its own values) or the global lines away from default. */
fun tomlPreview(editing: SettingsEditing, header: List<String>, none: String): String {
    val changed = SettingsSchema.allSettings.filter { it !is Setting.Action || it.name == "vulkan_lib_path" }.filter { editing.changed(it) }
    val bySection = changed.groupBy { it.section }.toSortedMap()
    return buildString {
        header.forEach { append("# ").append(it).append('\n') }
        if (bySection.isEmpty()) append("\n# ").append(none).append('\n')
        for ((section, list) in bySection) {
            append('\n').append('[').append(section).append("]\n")
            for (s in list.sortedBy { it.name }) append(s.name).append(" = ").append(tomlValue(s, editing.raw(s))).append('\n')
        }
    }
}

private fun tomlValue(s: Setting, raw: String): String = when (s) {
    is Setting.Bool -> ConfigValueShape.bool(ConfigValueShape.parseBool(raw, s.default))
    is Setting.IntRange -> ConfigValueShape.parseInt(raw, s.default).toString()
    is Setting.ListChoice -> if (raw.toDoubleOrNull() != null) raw else "\"$raw\""
    else -> "\"" + raw.replace("\\", "\\\\").replace("\"", "\\\"") + "\""
}

/** A preset: settings written together on one game, with undo. */
data class SettingsPreset(val id: String, val title: Int, val description: Int, val values: Map<String, String>)

object SettingsPresets {
    val all = listOf(
        SettingsPreset("perf", R.string.xd_preset_perf, R.string.xd_preset_perf_desc, mapOf(
            "GPU|draw_resolution_scale_x" to "1", "GPU|draw_resolution_scale_y" to "1",
            "Display|postprocess_scaling_and_sharpening" to "bilinear", "GPU|anisotropic_override" to "0",
            "Display|postprocess_antialiasing" to "none")),
        SettingsPreset("bal", R.string.xd_preset_bal, R.string.xd_preset_bal_desc, mapOf(
            "GPU|draw_resolution_scale_x" to "1", "GPU|draw_resolution_scale_y" to "1",
            "Display|postprocess_scaling_and_sharpening" to "fsr", "GPU|anisotropic_override" to "3")),
        SettingsPreset("qual", R.string.xd_preset_qual, R.string.xd_preset_qual_desc, mapOf(
            "GPU|draw_resolution_scale_x" to "2", "GPU|draw_resolution_scale_y" to "2",
            "Display|postprocess_scaling_and_sharpening" to "lanczos", "GPU|anisotropic_override" to "5",
            "GPU|draw_resolution_scale_threshold" to "160")),
    )

    /** How many values [preset] would change from what [editing] shows now. */
    fun changes(preset: SettingsPreset, editing: SettingsEditing): Int = preset.values.count { (key, value) ->
        val s = SettingsSchema.byKey[key] ?: return@count false
        ConfigValueShape.listOption((s as? Setting.ListChoice)?.options?.map { it.value }.orEmpty(), editing.raw(s)) != value &&
            editing.raw(s) != value
    }
}
