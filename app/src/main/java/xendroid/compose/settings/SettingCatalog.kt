package xendroid.compose.settings

import android.content.Context
import androidx.core.content.edit

/**
 * The redesign's view of the settings (docs/ui-redesign/bc, lote 1 and the game sheet): each
 * setting in one group, at one level, saying whether it also changes with the game open, whether
 * the app shows it for the first time, and what it depends on. Pure, so the JVM tests hold it:
 * every schema setting is placed exactly once.
 */
enum class SettingGroup { IMAGE, PERFORMANCE, AUDIO, INPUT, COMPAT, DRIVER, SYSTEM, DEBUG }

/** How many settings a screen lists: what players change, more, or every engine internal. */
enum class SettingLevel { ESSENTIAL, ADVANCED, ALL }

data class SettingMeta(
    val group: SettingGroup,
    val level: SettingLevel,
    /** Also changes while a game runs (the in-game menu sets it live). */
    val live: Boolean = false,
    /** A setting the core always had and the app shows now. */
    val isNew: Boolean = false,
    /** Values the list offers now that it did not before. */
    val newOptions: Set<String> = emptySet(),
    /** Only applies while setting [Dependency.key] has [Dependency.value]. */
    val dependsOn: Dependency? = null,
    /** A known limit worth a warning line under the row. */
    val warning: Warning? = null,
)

data class Dependency(val key: String, val value: String)

enum class Warning { NEEDS_BARYCENTRICS }

object SettingCatalog {
    private val E = SettingLevel.ESSENTIAL
    private val A = SettingLevel.ADVANCED
    private val X = SettingLevel.ALL

    private class Entry(val key: String, val level: SettingLevel, val live: Boolean = false, val isNew: Boolean = false,
                        val newOptions: Set<String> = emptySet(), val dependsOn: Dependency? = null, val warning: Warning? = null)

    private fun e(key: String, level: SettingLevel, live: Boolean = false, isNew: Boolean = false, newOptions: Set<String> = emptySet(),
                  dependsOn: Pair<String, String>? = null, warning: Warning? = null) =
        Entry(key, level, live, isNew, newOptions, dependsOn?.let { Dependency(it.first, it.second) }, warning)

    /** Each group's settings in the order the screens list them. */
    private val order: Map<SettingGroup, List<Entry>> = mapOf(
        SettingGroup.IMAGE to listOf(
            e("Display|postprocess_scaling_and_sharpening", E, live = true),
            e("GPU|draw_resolution_scale_x", E),
            e("GPU|draw_resolution_scale_y", E),
            e("GPU|anisotropic_override", E),
            e("Display|postprocess_antialiasing", E),
            e("Display|postprocess_dither", E),
            e("Display|present_letterbox", E),
            e("Console|widescreen", E),
            e("Console|internal_display_resolution", E, newOptions = setOf("17")),
            e("Video|internal_display_resolution_x", A, dependsOn = "Console|internal_display_resolution" to "17"),
            e("Video|internal_display_resolution_y", A, dependsOn = "Console|internal_display_resolution" to "17"),
            e("GPU|draw_resolution_scale_threshold", A),
            e("Display|postprocess_ffx_cas_additional_sharpness", A, dependsOn = "Display|postprocess_scaling_and_sharpening" to "cas"),
            e("Display|postprocess_ffx_fsr_sharpness_reduction", A, dependsOn = "Display|postprocess_scaling_and_sharpening" to "fsr"),
            e("Display|postprocess_ffx_fsr_max_upsampling_passes", A, dependsOn = "Display|postprocess_scaling_and_sharpening" to "fsr"),
            e("Kernel|kernel_display_gamma_type", A, newOptions = setOf("3")),
            e("Kernel|kernel_display_gamma_power", A, dependsOn = "Kernel|kernel_display_gamma_type" to "3"),
            e("Display|present_safe_area_x", A),
            e("Display|present_safe_area_y", A),
            e("GPU|draw_resolution_scaled_texture_offsets", X),
            e("GPU|resolve_resolution_scale_fill_half_pixel_offset", X),
            e("Display|present_render_pass_clear", X),
        ),
        SettingGroup.PERFORMANCE to listOf(
            e("GPU|framerate_limit", E, live = true),
            e("GPU|guest_display_refresh_cap", E),
            e("GPU|async_shader_compilation", A),
            e("GPU|async_shader_vs_interpreter", A, dependsOn = "GPU|async_shader_compilation" to "true"),
            e("GPU|async_shader_skip_draws", A),
            e("GPU|pipeline_storage_precreate", A),
            e("GPU|store_shaders", A),
            e("Kernel|precise_guest_delays", A),
            e("APU|apu_performance_hint", A),
            e("Vulkan|vulkan_pipeline_creation_threads", A),
            e("GPU|vulkan_mid_frame_submission_draws", X),
            e("Kernel|guest_scheduler", X),
            e("Kernel|guest_scheduler_quantum_us", X, dependsOn = "Kernel|guest_scheduler" to "true"),
            e("GPU|texture_cache_memory_limit_soft", X),
            e("GPU|texture_cache_memory_limit_hard", X),
            e("GPU|gpu_stall_spin_iterations", X),
            e("Kernel|ignore_thread_affinities", X),
            e("Kernel|ignore_thread_priorities", X),
            e("General|time_scalar", X),
        ),
        SettingGroup.AUDIO to listOf(
            e("APU|volume", E, live = true),
            e("APU|apu_aaudio_buffer_bursts", A),
            e("APU|apu_aaudio_adaptive_buffer", A),
            e("APU|apu", X),
            e("APU|xma_decoder", X),
            e("APU|use_dedicated_xma_thread", X),
            e("APU|apu_max_queued_frames", X),
            e("APU|apu_pump_topup", X),
            e("APU|enable_xmp", X),
            e("Console|xmp_default_volume", X),
        ),
        SettingGroup.INPUT to listOf(
            e("HID|show_touch_overlay", E, live = true),
            e("HID|vibration", E),
            e("HID|left_stick_deadzone_percentage", E),
            e("HID|right_stick_deadzone_percentage", E),
            e("HID|guide_button", A),
            e("HID|hid", X),
        ),
        SettingGroup.COMPAT to listOf(
            e("GPU|clear_memory_page_state", A),
            e("GPU|depth_float24_convert_in_pixel_shader", A),
            e("GPU|occlusion_query", A),
            e("GPU|readback_resolve_sync", A),
            e("GPU|precise_interpolation", A, warning = Warning.NEEDS_BARYCENTRICS),
            e("GPU|depth_bias_shader_offset", A),
            e("GPU|readback_resolve", X),
            e("GPU|memexport_enable", X),
            e("Vulkan|vulkan_allow_reverse_z", X),
            e("Kernel|stack_size_multiplier_hack", X),
            e("CPU|collapse_memory_delay_spins", X),
            e("GPU|depth_float24_round", X),
            e("GPU|depth_transfer_not_equal_test", X),
            e("GPU|half_pixel_offset", X),
            e("GPU|native_2x_msaa", X),
            e("GPU|render_target_path", X),
            e("GPU|snorm16_render_target_full_range", X),
            e("GPU|non_seamless_cube_map", X),
            e("GPU|texture_gradient_exp_bias", X),
            e("GPU|texture_integer_num_format", X),
            e("GPU|accurate_resolve_number_formats", X),
            e("GPU|resolve_copy_dest_number_packing", X),
            e("GPU|mrt_edram_used_range_clamp_to_min", X),
            e("GPU|gpu_allow_invalid_fetch_constants", X),
            e("GPU|force_convert_quad_lists_to_triangle_lists", X),
            e("GPU|force_convert_triangle_fans_to_lists", X),
            e("GPU|execute_unclipped_draw_vs_on_cpu", X),
            e("GPU|execute_unclipped_draw_vs_on_cpu_with_scissor", X),
            e("GPU|execute_unclipped_draw_vs_on_cpu_for_psi_render_backend", X),
            e("GPU|value_convert_7e3_8888_reuse", X),
            e("CPU|inline_mmio_access", X),
            e("CPU|disable_context_promotion", X),
            e("CPU|store_all_context_values", X),
            e("CPU|clock_no_scaling", X),
            e("CPU|clock_source_raw", X),
            e("Memory|mmap_address_high", X),
            e("Memory|protect_zero", X),
            e("Memory|protect_on_release", X),
            e("Memory|scribble_heap", X),
            e("Memory|writable_executable_memory", X),
            e("Memory|ignore_offset_for_ranged_allocations", X),
        ),
        SettingGroup.DRIVER to listOf(
            e("Vulkan|vulkan_lib_path", E),
            e("Vulkan|vulkan_dynamic_rendering", X),
            e("Vulkan|vulkan_avoid_geometry_shaders", X),
            e("Vulkan|vulkan_depth_unorm24", X),
            e("Vulkan|vulkan_bc_to_etc2", X),
            e("Vulkan|vulkan_sparse_shared_memory", X),
            e("Vulkan|vulkan_dynamic_pipeline_state", X),
            e("Vulkan|vulkan_async_skip_draws", X),
            e("Vulkan|vulkan_placeholder_pipelines", X),
            e("Vulkan|vulkan_allow_present_mode_immediate", X),
            e("Vulkan|vulkan_allow_present_mode_mailbox", X),
            e("Vulkan|vulkan_allow_present_mode_fifo_relaxed", X),
            e("Vulkan|vulkan_in_pass_resolve", X),
            e("Vulkan|vulkan_resolve_to_texture", X),
            e("Vulkan|vulkan_resolve_to_texture_promote", X),
            e("Vulkan|vulkan_resolve_to_texture_serve", X),
            e("Vulkan|vulkan_cache_texture_descriptors", X),
            e("Vulkan|vulkan_texture_descriptor_reuse_edge", X, dependsOn = "Vulkan|vulkan_cache_texture_descriptors" to "true"),
            e("GPU|gpu", X),
        ),
        SettingGroup.SYSTEM to listOf(
            e("Console|user_language", E),
            e("Console|user_country", E),
            e("General|apply_patches", E),
            e("UI|android_soft_keyboard", E),
            e("UI|android_message_box", E),
            e("UI|show_achievement_notification", E),
            e("UI|achievement_notification_position_by_game", A),
            e("General|launch_module", A),
            e("Storage|mount_memory_unit", A),
            e("Kernel|apply_title_update", A),
            e("Kernel|allow_incompatible_title_update", A),
            e("Kernel|network_enabled", A),
            e("Content|license_mask", A),
            e("Console|video_standard", A),
            e("Console|use_50Hz_mode", A),
            e("Kernel|console_type", X),
            e("Kernel|cl", X),
            e("General|guest_crash_is_fatal", X),
            e("Video|avpack", X),
            e("Video|interlaced", X),
            e("Video|enable_3d_mode", X),
            e("UI|storage_selection_dialog", X),
            e("UI|headless", X),
            e("Storage|mount_scratch", X),
            e("Storage|mount_cache", X),
            e("General|allow_plugins", X),
            e("Kernel|staging_mode", X),
        ),
        SettingGroup.DEBUG to listOf(
            e("Logging|dump_session_logs", E),
            e("Logging|log_level", X),
            e("Logging|log_mask", X),
            e("Display|show_debug_overlay", X),
            e("Logging|log_sessions_keep", X),
            e("Logging|flush_log", X),
            e("Logging|log_string_format_kernel_calls", X),
            e("Logging|log_high_frequency_kernel_calls", X),
            e("Vulkan|vulkan_log_debug_messages", X),
            e("Vulkan|vulkan_validation", X),
            e("Vulkan|vulkan_renderdoc_capture", X),
            e("Vulkan|vulkan_debug_gpu_probe", X),
            e("GPU|log_ringbuffer_kickoff_initiator_bts", X),
            e("GPU|log_guest_driven_gpu_register_written_values", X),
            e("GPU|trace_gpu_stream", X),
            e("Kernel|guest_scheduler_stats", X),
            e("Kernel|kernel_pix", X),
            e("Kernel|kernel_cert_monitor", X),
            e("Kernel|kernel_debug_monitor", X),
            e("CPU|validate_hir", X),
            e("CPU|trace_function_references", X),
            e("CPU|trace_function_coverage", X),
            e("CPU|break_condition_truncate", X),
            e("CPU|break_on_unimplemented_instructions", X),
            e("CPU|break_on_start", X),
            e("CPU|log_delay_collapse_rejects", X),
            e("CPU|log_safepoint_pc", X),
            e("APU|ffmpeg_verbose", X),
        ),
    )

    private val metas: Map<String, SettingMeta> = order.flatMap { (group, list) ->
        list.map { it.key to SettingMeta(group, it.level, it.live, it.isNew, it.newOptions, it.dependsOn, it.warning) }
    }.toMap()

    /** Every key the catalog places, in group order (the tests compare it with the schema). */
    val placedKeys: List<String> = order.values.flatten().map { it.key }

    val groups: List<SettingGroup> = SettingGroup.entries

    fun meta(key: String): SettingMeta = metas[key] ?: SettingMeta(SettingGroup.DEBUG, SettingLevel.ALL)

    fun meta(s: Setting): SettingMeta = meta(s.key)

    /** [group]'s settings shown at [level], in screen order. */
    fun settings(group: SettingGroup, level: SettingLevel = SettingLevel.ALL): List<Setting> =
        order.getValue(group).filter { it.level <= level }.mapNotNull { SettingsSchema.byKey[it.key] }

    /** All settings shown at [level], group by group. */
    fun settings(level: SettingLevel): List<Setting> = groups.flatMap { settings(it, level) }

    val newCount: Int get() = metas.values.count { it.isNew }
    val newOptionCount: Int get() = metas.values.sumOf { it.newOptions.size }

    /**
     * Whether [s] applies with the values read by [valueOf] (a raw string per key): a setting
     * that depends on another applies only while that one has the needed value.
     */
    fun applies(s: Setting, valueOf: (String) -> String?): Boolean {
        val dep = meta(s).dependsOn ?: return true
        val raw = valueOf(dep.key) ?: return true
        return same(dep.key, raw, dep.value)
    }

    private fun same(key: String, raw: String, wanted: String): Boolean {
        if (raw == wanted) return true
        val s = SettingsSchema.byKey[key]
        if (s is Setting.Bool) return ConfigValueShape.parseBool(raw, s.default).toString() == wanted
        val a = raw.toDoubleOrNull(); val b = wanted.toDoubleOrNull()
        return a != null && b != null && kotlin.math.abs(a - b) < 1e-6
    }
}

/**
 * The level the settings screens list (Settings → App → Interface). Kept apart from [UiModeStore]
 * but written with it: the game process still reads Player/Developer when a game starts.
 */
object SettingLevelStore {
    private const val PREFS = "ui_look"
    private const val KEY = "settings_level"

    fun read(context: Context): SettingLevel {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        SettingLevel.entries.firstOrNull { it.name == prefs.getString(KEY, null) }?.let { return it }
        return when {
            !UiModeStore.isChosen(context) -> SettingLevel.ADVANCED
            UiModeStore.read(context) == UiMode.PLAYER -> SettingLevel.ESSENTIAL
            else -> SettingLevel.ALL
        }
    }

    fun write(context: Context, level: SettingLevel) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit { putString(KEY, level.name) }
        UiModeStore.write(context, if (level == SettingLevel.ESSENTIAL) UiMode.PLAYER else UiMode.DEVELOPER)
    }
}

/** The settings pinned to the quick settings of the game sheet and the library panel. */
object PinnedSettings {
    private const val PREFS = "ui_look"
    private const val KEY = "pinned_settings"
    /** The resolution scale as one row (width and height together). */
    const val RESOLUTION = "@resolution"
    val DEFAULT = listOf("GPU|framerate_limit", RESOLUTION, "Display|postprocess_scaling_and_sharpening",
        "GPU|anisotropic_override", "Vulkan|vulkan_lib_path", "APU|volume")

    fun read(context: Context): List<String> {
        val raw = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, null) ?: return DEFAULT
        return raw.split(',').filter { it == RESOLUTION || it in SettingsSchema.byKey }
    }

    fun write(context: Context, keys: List<String>) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit { putString(KEY, keys.distinct().joinToString(",")) }
    }

    fun toggled(keys: List<String>, key: String): List<String> = if (key in keys) keys - key else keys + key
}
