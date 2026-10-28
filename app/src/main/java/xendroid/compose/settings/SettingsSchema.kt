package xendroid.compose.settings

import xendroid.compose.settings.Setting.*

private fun desc(n: String) = SettingDescriptions.byName[n] ?: ""
private fun b(s: String, n: String, t: String, d: Boolean) = Setting.Bool(s, n, t, d, desc(n))
private fun i(s: String, n: String, t: String, d: Int, lo: Int, hi: Int) =
    Setting.IntRange(s, n, t, d, lo, hi, desc(n))
private fun l(s: String, n: String, t: String, d: String, vararg o: Pair<String, String>) =
    Setting.ListChoice(s, n, t, d, o.map { ListOption(it.first, it.second) }, desc(n))
private fun t(s: String, n: String, t: String, d: String, placeholder: String) = Setting.Text(s, n, t, d, placeholder, desc(n))

object SettingsSchema {

    val categories: List<SettingsCategory> = listOf(

        SettingsCategory("Vulkan", listOf(
            b("Vulkan", "vulkan_sparse_shared_memory", "Sparse shared memory", true),
            b("Vulkan", "vulkan_log_debug_messages", "Log debug messages", false),
            b("Vulkan", "vulkan_validation", "Validation layers", false),
            b("Vulkan", "vulkan_renderdoc_capture", "RenderDoc capture layer", false),
            b("Vulkan", "vulkan_allow_present_mode_immediate", "Allow present mode: immediate", true),
            b("Vulkan", "vulkan_allow_present_mode_mailbox", "Allow present mode: mailbox", false),
            b("Vulkan", "vulkan_allow_present_mode_fifo_relaxed", "Allow present mode: FIFO relaxed", false),
            b("Vulkan", "vulkan_async_skip_draws", "Async skip draws", true),
            b("Vulkan", "vulkan_placeholder_pipelines", "Placeholder pipelines", true),
            b("Vulkan", "vulkan_dynamic_pipeline_state", "Extended dynamic state", false),
            // A slider, not a dropdown: 0 = synchronous, 1..5 = explicit. The native cvar also
            // accepts -1 (auto), but a 0..5 slider is the intended UX.
            i("Vulkan", "vulkan_pipeline_creation_threads", "Pipeline creation threads", 4, 0, 5),
            Action("Vulkan", "vulkan_lib_path", "Custom Vulkan driver", "", desc("vulkan_lib_path")),
            b("Vulkan", "vulkan_allow_reverse_z", "Reverse depth ranges in the driver", true),
            b("Vulkan", "vulkan_dynamic_rendering", "Dynamic rendering", true),
            b("Vulkan", "vulkan_avoid_geometry_shaders", "Avoid geometry shaders", true),
            b("Vulkan", "vulkan_depth_unorm24", "Native D24 depth", true),
            b("Vulkan", "vulkan_bc_to_etc2", "Transcode Textures", true),
            b("Vulkan", "vulkan_in_pass_resolve", "In-pass EDRAM resolve", true),
            b("Vulkan", "vulkan_resolve_to_texture_promote", "Resolve-to-texture: promote", true),
            b("Vulkan", "vulkan_resolve_to_texture", "Resolve-to-texture: store", true),
            b("Vulkan", "vulkan_resolve_to_texture_serve", "Resolve-to-texture: skip upload", true),
            // Master toggle gates cross-draw descriptor-set reuse; the edge toggle (only when
            // reuse is on) picks edge's bitmask gate vs the content-hash gate for A/B.
            b("Vulkan", "vulkan_cache_texture_descriptors", "Cache texture descriptors", true),
            b("Vulkan", "vulkan_texture_descriptor_reuse_edge", "Texture descriptor gate: edge (off = hash)", false),
        )),

        SettingsCategory("Video", listOf(
            b("Console", "widescreen", "Widescreen", true),
            l("Console", "video_standard", "Video standard", "1",
                "1" to "NTSC", "2" to "NTSC-J", "3" to "PAL-60"),
            l("Console", "internal_display_resolution", "Display mode reported to game", "8",
                "0" to "640x480", "1" to "640x576", "2" to "720x480", "3" to "720x576",
                "4" to "800x600", "5" to "848x480", "6" to "1024x768", "7" to "1152x864",
                "8" to "1280x720", "9" to "1280x768", "10" to "1280x960", "11" to "1280x1024",
                "12" to "1360x768", "13" to "1440x900", "14" to "1680x1050", "15" to "1920x540",
                "16" to "1920x1080", "17" to "Custom (width and height below)"),
            i("Video", "internal_display_resolution_x", "Custom display width", 1280, 1, 1920),
            i("Video", "internal_display_resolution_y", "Custom display height", 720, 1, 1080),
            b("Console", "use_50Hz_mode", "Use 50Hz mode", false),
            l("Video", "avpack", "AV pack", "8",
                "0" to "PAL-60 Component (SD)", "1" to "Unused", "2" to "PAL-60 SCART",
                "3" to "480p Component (HD)", "4" to "HDMI+A", "5" to "PAL-60 Composite/S-Video",
                "6" to "VGA", "7" to "TV PAL-60", "8" to "HDMI"),
            b("Video", "interlaced", "Interlaced", false),
            b("Video", "enable_3d_mode", "Enable 3D mode", false),
        )),

        SettingsCategory("UI", listOf(
            b("UI", "show_achievement_notification", "Show achievement notification", true),
            b("UI", "achievement_notification_position_by_game", "Achievement where the game places it", false),
            b("UI", "storage_selection_dialog", "Storage selection dialog", false),
            b("UI", "headless", "Headless", true),
            b("UI", "android_soft_keyboard", "Android keyboard for game text input", true),
            b("UI", "android_message_box", "Android dialog for game message boxes", true),
        )),

        SettingsCategory("Storage", listOf(
            b("Storage", "mount_scratch", "Mount scratch", false),
            b("Storage", "mount_cache", "Mount cache", false),
            b("Storage", "mount_memory_unit", "Mount memory unit (MU)", false),
        )),

        SettingsCategory("Kernel", listOf(
            b("Kernel", "staging_mode", "Staging mode", false),
            // Takes effect on the next game launch.
            b("Kernel", "guest_scheduler", "Guest scheduler (cooperative fibers)", true),
            // Only meaningful with the guest scheduler on.
            i("Kernel", "guest_scheduler_quantum_us", "Guest scheduler quantum (us)", 1000, 250, 8000),
            b("Kernel", "guest_scheduler_stats", "Guest scheduler stats logging", false),
            b("Logging", "log_high_frequency_kernel_calls", "Log high-frequency kernel calls", false),
            l("Kernel", "kernel_display_gamma_type", "Display gamma type", "2",
                "0" to "linear", "1" to "sRGB (CRT)", "2" to "BT.709 (HDTV)", "3" to "power (custom gamma below)"),
            // A TOML double: every option keeps one '.'; the core's default is 2.22222233.
            l("Kernel", "kernel_display_gamma_power", "Custom display gamma", "2.22222233",
                "1.8" to "1.8", "2.0" to "2.0", "2.2" to "2.2", "2.22222233" to "2.22", "2.4" to "2.4", "2.6" to "2.6", "2.8" to "2.8"),
            b("Kernel", "precise_guest_delays", "Precise guest delays", true),
            i("Kernel", "stack_size_multiplier_hack", "Stack size multiplier (hack)", 1, 1, 8),
            l("Kernel", "console_type", "Console type", "-1", "-1" to "Retail", "0" to "Development kit", "1" to "Test kit"),
            t("Kernel", "cl", "Extra command line for the game", "", ""),
            b("Kernel", "ignore_thread_affinities", "Ignore thread affinities", true),
            b("Kernel", "kernel_pix", "Kernel PIX", false),
            b("Kernel", "kernel_cert_monitor", "Kernel cert monitor", false),
            b("Kernel", "ignore_thread_priorities", "Ignore thread priorities", true),
            b("Kernel", "allow_incompatible_title_update", "Allow incompatible title update", true),
            b("Kernel", "apply_title_update", "Apply title update", true),
            b("Kernel", "kernel_debug_monitor", "Kernel debug monitor", false),
            b("Kernel", "network_enabled", "Networking", true),
        )),

        SettingsCategory("Controller", listOf(
            b("HID", "show_touch_overlay", "Show on-screen controller", true),
            b("HID", "vibration", "Controller vibration", true),
            b("HID", "guide_button", "Send the Guide button to games", true),
            // Applied by InputSystem::GetState to every player and input driver, on top of
            // the app's own 8% cut per axis: raises the dead zone (a drifting stick).
            l("HID", "left_stick_deadzone_percentage", "Left stick dead zone", "0.0", *DEAD_ZONES),
            l("HID", "right_stick_deadzone_percentage", "Right stick dead zone", "0.0", *DEAD_ZONES),
        )),

        SettingsCategory("HID", listOf(
            l("HID", "hid", "HID backend", "android",
                "android" to "android", "nop" to "nop"),
        )),

        SettingsCategory("Memory", listOf(
            i("Memory", "mmap_address_high", "mmap address high", 8, 2, 63),
            b("Memory", "scribble_heap", "Scribble heap", false),
            b("Memory", "protect_zero", "Protect zero page", true),
            b("Memory", "writable_executable_memory", "Writable executable memory", true),
            b("Memory", "protect_on_release", "Protect on release", false),
            b("Memory", "ignore_offset_for_ranged_allocations", "Ignore offset for ranged allocations", false),
        )),

        SettingsCategory("XConfig", listOf(
            l("Console", "user_language", "User language", "1",
                "1" to "en", "2" to "ja", "3" to "de", "4" to "fr", "5" to "es", "6" to "it",
                "7" to "ko", "8" to "zh", "9" to "pt", "11" to "pl", "12" to "ru", "13" to "sv",
                "14" to "tr", "15" to "nb", "16" to "nl", "17" to "zh"), // value 10 skipped; 8 & 17 both zh
            l("Console", "user_country", "User country", "103", *userCountryOptions()),
        )),

        SettingsCategory("Display", listOf(
            b("Display", "present_letterbox", "Present letterbox", true),
            // The core's default (false): with no value saved, this is what applies.
            b("Display", "postprocess_dither", "Postprocess dither", false),
            l("Display", "postprocess_scaling_and_sharpening", "Scaling & sharpening", "",
                "bilinear" to "bilinear", "cas" to "cas", "fsr" to "fsr",
                "sgsr" to "sgsr (experimental)",
                "lanczos" to "lanczos (sharp, no halos)",
                "crt" to "crt (scanlines)"),  // "" => bilinear (no selection)
            l("Display", "postprocess_ffx_cas_additional_sharpness", "CAS: extra sharpness", "0.0",
                "0.0" to "0.0", "0.25" to "0.25", "0.5" to "0.5", "0.75" to "0.75", "1.0" to "1.0"),
            l("Display", "postprocess_ffx_fsr_sharpness_reduction", "FSR: sharpness reduction", "0.2",
                "0.0" to "0.0", "0.1" to "0.1", "0.2" to "0.2", "0.5" to "0.5", "1.0" to "1.0", "2.0" to "2.0"),
            i("Display", "postprocess_ffx_fsr_max_upsampling_passes", "FSR: upsampling passes", 1, 1, 4),
            // Letterboxed picture: how much of the width / height must stay; below 100 the
            // edges may be cut instead of drawing bars (the presenter allows the cut-off).
            i("Display", "present_safe_area_x", "Keep at least this much of the width (%)", 100, 50, 100),
            i("Display", "present_safe_area_y", "Keep at least this much of the height (%)", 100, 50, 100),
            b("Display", "present_render_pass_clear", "Present render-pass clear", true),
            l("Display", "postprocess_antialiasing", "Antialiasing", "",
                "none" to "none", "fxaa" to "fxaa", "fxaa_extreme" to "fxaa_extreme"), // "" => none
            // host_present_from_non_ui_thread intentionally NOT exposed: forced true on Android
            // (false black-screens the app), so there is no valid user choice to make.
            b("Display", "show_debug_overlay", "Show debug overlay", false),
        )),

        SettingsCategory("GPU", listOf(
            // A dropdown, not a slider: only a few values are meaningful, and "unlimited"
            // needs to be an explicit choice rather than the bottom of a range.
            l("GPU", "framerate_limit", "Frame rate limit", "60",
                "60" to "60 FPS", "30" to "30 FPS", "45" to "45 FPS",
                "90" to "90 FPS", "120" to "120 FPS", "0" to "Unlimited"),
            b("GPU", "guest_display_refresh_cap", "Cap guest display refresh (VSync)", true),
            // vulkan_texture_cache: -1 leaves each sampler as the game set it.
            l("GPU", "anisotropic_override", "Anisotropic filtering", "-1",
                "-1" to "Game decides", "0" to "Off", "1" to "1x", "2" to "2x", "3" to "4x", "4" to "8x", "5" to "16x"),
            // vulkan_pipeline_cache: off creates pipelines on the spot (stutter, no artifacts).
            b("GPU", "async_shader_compilation", "Asynchronous shader compilation", true),
            b("GPU", "async_shader_vs_interpreter", "Interpret vertex shaders while they compile", true),
            b("GPU", "async_shader_skip_draws", "Skip draws until their shader is ready", true),
            b("GPU", "pipeline_storage_precreate", "Create stored pipelines when a game starts", true),
            b("GPU", "store_shaders", "Store shaders", true),
            b("GPU", "resolve_resolution_scale_fill_half_pixel_offset", "Resolve scale: fill half-pixel offset", true),
            // uma = no copy, the CPU reads host-mapped shared memory directly; the only mode
            // that works on Adreno, which cannot import guest RAM. Retired some/full parse as uma.
            l("GPU", "readback_resolve", "Readback resolve", "uma",
                "uma" to "UMA (direct map, no copy)",
                "fast" to "Fast (copy CPU-read resolves)", "all" to "All (copy every resolve)",
                "none" to "None (disabled)"),
            b("GPU", "readback_resolve_sync", "Wait for resolve readbacks", true),
            b("GPU", "precise_interpolation", "Precise interpolation", true),
            b("GPU", "depth_bias_shader_offset", "Decal depth bias in the shader", false),
            b("GPU", "memexport_enable", "Memory export visible to the CPU", false),
            // 'fake' fabricates a result (fastest; some effects may look slightly wrong);
            // 'fast'/'fast-alt' issue async queries without stalling; 'strict' stalls the
            // command thread until the GPU answers. Live (SetZPDMode) -- no relaunch needed.
            l("GPU", "occlusion_query", "Occlusion query mode", "fast-alt",
                "fake" to "Fake (no GPU query, fastest)", "fast" to "Fast (async query, no stall)",
                "fast-alt" to "Fast-alt (async, precise zeros)", "strict" to "Strict (real query, stalls)"),
            // Mid-frame command-buffer split: end+submit every N draws so the GPU overlaps
            // rendering with CPU command-building instead of idling until swap. 0 = off;
            // too-small values hurt tiled GPUs.
            i("GPU", "vulkan_mid_frame_submission_draws", "Mid-frame submission (draws, 0=off)", 1300, 0, 4096),
            b("GPU", "snorm16_render_target_full_range", "snorm16 render target full range", true),
            // min == the real TOML default (384); a higher floor would silently coerce the default up.
            i("GPU", "texture_cache_memory_limit_soft", "Texture cache soft limit (MB)", 384, 384, 4096),
            b("GPU", "native_2x_msaa", "Native 2x MSAA", true),
            l("GPU", "render_target_path", "Render target path", "performance",
                "performance" to "performance", "accuracy" to "accuracy"),
            b("GPU", "half_pixel_offset", "Half-pixel offset", true),
            // Upstream accuracy features that only a few titles need but cost shader performance
            // in every title. Off = pre-upstream behaviour.
            b("GPU", "texture_gradient_exp_bias", "Accuracy: per-axis gradient LOD bias", false),
            b("GPU", "texture_integer_num_format", "Accuracy: integer num_format texture scaling", false),
            b("GPU", "accurate_resolve_number_formats", "Accuracy: resolve number formats and gamma", false),
            b("GPU", "resolve_copy_dest_number_packing", "Accuracy: full-resolve destination packing", false),
            b("GPU", "log_ringbuffer_kickoff_initiator_bts", "Log ringbuffer kickoff initiator BTs", false),
            i("GPU", "texture_cache_memory_limit_hard", "Texture cache hard limit (MB)", 768, 512, 4096),
            i("GPU", "gpu_stall_spin_iterations", "GPU stall spin iterations", 32, 0, 512),
            b("GPU", "gpu_allow_invalid_fetch_constants", "Allow invalid fetch constants", true),
            b("GPU", "log_guest_driven_gpu_register_written_values", "Log guest-driven GPU register writes", false),
            i("Vulkan", "vulkan_debug_gpu_probe", "GPU cost probe, bitmask (breaks image)", 0, 0, 63),
            b("GPU", "trace_gpu_stream", "Trace GPU stream", false),
            b("GPU", "force_convert_quad_lists_to_triangle_lists", "Convert quad lists to triangle lists", false),
            b("GPU", "execute_unclipped_draw_vs_on_cpu_with_scissor", "Unclipped draw VS on CPU (scissor)", false),
            b("GPU", "mrt_edram_used_range_clamp_to_min", "MRT EDRAM used-range clamp to min", true),
            b("GPU", "execute_unclipped_draw_vs_on_cpu", "Unclipped draw VS on CPU", true),
            b("GPU", "force_convert_triangle_fans_to_lists", "Convert triangle fans to lists", false),
            b("GPU", "non_seamless_cube_map", "Non-seamless cube map", true),
            b("GPU", "depth_float24_round", "Depth float24 round", false),
            b("GPU", "clear_memory_page_state", "Clear memory page state", false),
            b("GPU", "depth_transfer_not_equal_test", "Depth transfer not-equal test", true),
            b("GPU", "execute_unclipped_draw_vs_on_cpu_for_psi_render_backend", "Unclipped draw VS on CPU (PSI backend)", true),
            l("GPU", "draw_resolution_scale_x", "Resolution scale, width (real)", "1",
                "-1" to "0.5x", "-2" to "0.75x", "1" to "1x", "2" to "2x", "3" to "3x"),
            l("GPU", "draw_resolution_scale_y", "Resolution scale, height (real)", "1",
                "-1" to "0.5x", "-2" to "0.75x", "1" to "1x", "2" to "2x", "3" to "3x"),
            l("GPU", "draw_resolution_scale_threshold", "Keep narrow surfaces at native size", "0",
                "0" to "Off", "80" to "up to 80 px", "160" to "up to 160 px", "320" to "up to 320 px", "640" to "up to 640 px"),
            b("GPU", "draw_resolution_scaled_texture_offsets", "Draw resolution-scaled texture offsets", true),
            l("GPU", "gpu", "GPU backend", "vulkan",
                "vulkan" to "vulkan", "null" to "null"),
            b("GPU", "depth_float24_convert_in_pixel_shader", "Depth float24 convert in pixel shader", false),
            b("GPU", "value_convert_7e3_8888_reuse", "value_convert_7e3_8888_reuse", true),
        )),

        SettingsCategory("CPU", listOf(
            b("CPU", "validate_hir", "Validate HIR", false),
            b("CPU", "trace_function_references", "Trace function references", false),
            b("CPU", "trace_function_coverage", "Trace function coverage", false),
            b("CPU", "store_all_context_values", "Store all context values", false),
            b("CPU", "break_condition_truncate", "Break condition truncate", true),
            b("CPU", "clock_source_raw", "Clock source raw", false),
            b("CPU", "break_on_unimplemented_instructions", "Break on unimplemented instructions", true),
            // NOTE: your original paste cut off mid-line here ("break_on_star"). Restored as
            // break_on_start to match the key SettingCatalog.kt already lists
            // (e("CPU|break_on_start", X)) -- PLEASE VERIFY this one line (title/default) against
            // your actual file; everything else in this file is exactly what you provided.
            b("CPU", "break_on_start", "Break on start", false),
        )),

        SettingsCategory("Logging", listOf(
            b("Logging", "log_string_format_kernel_calls", "Log string-format kernel calls", false),
            l("Logging", "log_level", "Log level", "2",
                "0" to "error", "1" to "warning", "2" to "info", "3" to "debug"),
            // 15i: Xenia's log_mask turns categories off (kernel 1, audio 2, CPU 4, GPU 8); lines
            // with no category always stay. "Only X" keeps one subsystem's lines.
            l("Logging", "log_mask", "Log filter", "0",
                "0" to "everything", "7" to "only the GPU", "13" to "only audio", "14" to "only the kernel",
                "11" to "only the CPU", "1" to "all but the kernel", "8" to "all but the GPU"),
            b("Logging", "flush_log", "Flush log", true),
            i("Logging", "log_sessions_keep", "Shelved log sessions to keep", 4, 1, 16),
            Action("Logging", "dump_session_logs", "Export session logs to Downloads", "", desc("dump_session_logs")),
        )),

        SettingsCategory("Content", listOf(
            l("Content", "license_mask", "License mask", "0",
                "0" to "disable", "1" to "first", "-1" to "all"),
        )),

        SettingsCategory("General", listOf(
            i("General", "time_scalar", "Time scalar", 1, 1, 8),
            b("General", "allow_plugins", "Allow plugins", false),
            b("General", "apply_patches", "Apply patches", true),
            t("General", "launch_module", "Executable to launch", "", "default.xex"),
            b("General", "guest_crash_is_fatal", "End the game when a guest thread crashes", true),
        )),

        SettingsCategory("APU", listOf(
            b("APU", "apu_pump_topup", "Audio pump top-up", true),
            i("APU", "volume", "Volume", 100, 0, 100),
            b("APU", "apu_performance_hint", "Audio performance hint (ADPF)", true),
            i("Console", "xmp_default_volume", "XMP default volume", 70, 0, 100),
            b("APU", "ffmpeg_verbose", "FFmpeg verbose", false),
            i("APU", "apu_max_queued_frames", "Max queued frames", 8, 4, 64),
            i("APU", "apu_aaudio_buffer_bursts", "Audio buffer depth", 2, 2, 8),
            // 15j: grows after underruns, back to the depth above after quiet stretches.
            b("APU", "apu_aaudio_adaptive_buffer", "Adaptive audio buffer", false),
            b("APU", "enable_xmp", "Enable XMP", true),
            l("APU", "xma_decoder", "XMA decoder", "new",
                "new" to "new", "old" to "old", "master" to "master", "fake" to "fake"),
            b("APU", "use_dedicated_xma_thread", "Use dedicated XMA thread", true),
            l("APU", "apu", "APU backend", "aaudio",
                "nop" to "nop", "aaudio" to "aaudio", "opensles" to "opensles"),
        )),
    )

    val allSettings: List<Setting> = categories.flatMap { it.settings }
    val byKey: Map<String, Setting> = allSettings.associateBy { it.key }

    /**
     * L02, Player mode: the settings a player changes, in this order. Engine internals,
     * accuracy hacks, logging and experiments stay in Developer mode; nothing is lost, the
     * stored values of hidden settings are kept and still apply.
     */
    val playerKeys: List<String> = listOf(
        "GPU|framerate_limit", "GPU|guest_display_refresh_cap",
        "GPU|draw_resolution_scale_x", "GPU|draw_resolution_scale_y",
        "Display|postprocess_scaling_and_sharpening", "Display|postprocess_antialiasing", "Display|present_letterbox",
        "Display|postprocess_dither",
        "GPU|anisotropic_override",
        "Console|widescreen", "Console|internal_display_resolution",
        "Vulkan|vulkan_lib_path",
        "HID|show_touch_overlay", "HID|left_stick_deadzone_percentage", "HID|right_stick_deadzone_percentage",
        "UI|android_soft_keyboard", "UI|android_message_box",
        "UI|show_achievement_notification",
        "Console|user_language", "Console|user_country",
        "APU|volume", "General|apply_patches",
        "Logging|dump_session_logs",
    )

    fun categoriesFor(mode: UiMode): List<SettingsCategory> = when (mode) {
        UiMode.DEVELOPER -> categories
        UiMode.PLAYER -> listOf(SettingsCategory("Essentials", playerKeys.mapNotNull(byKey::get)))
    }
}

// Stick dead zones, a share of full deflection; stored as TOML doubles (one '.').
private val DEAD_ZONES: Array<Pair<String, String>> = arrayOf(
    "0.0" to "Off", "0.05" to "5%", "0.1" to "10%", "0.15" to "15%", "0.2" to "20%", "0.25" to "25%", "0.3" to "30%",
)

// user_country: values 1..109 with 17 and 94 skipped (107 entries). Order matches
// arrays.xml (es_arr_xconfig_user_country). Default 103 = US.
private val USER_COUNTRY_ISO: List<String> = listOf(
    "AE","AL","AM","AR","AT","AU","AZ","BE","BG","BH","BN","BO","BR","BY","BZ","CA",
    "CH","CL","CN","CO","CR","CZ","DE","DK","DO","DZ","EC","EE","EG","ES","FI","FO",
    "FR","GB","GE","GR","GT","HK","HN","HR","HU","ID","IE","IL","IN","IQ","IR","IS",
    "IT","JM","JO","JP","KE","KG","KR","KW","KZ","LB","LI","LT","LU","LV","LY","MA",
    "MC","MK","MN","MO","MV","MX","MY","NI","NL","NO","NZ","OM","PA","PE","PH","PK",
    "PL","PR","PT","PY","QA","RO","RU","SA","SE","SG","SI","SK","SV","SY","TH","TN",
    "TR","TT","TW","UA","US","UY","UZ","VE","VN","YE","ZA",
)

private fun userCountryOptions(): Array<Pair<String, String>> {
    val out = ArrayList<Pair<String, String>>(107)
    var iso = 0
    var value = 1
    while (iso < USER_COUNTRY_ISO.size) {
        if (value == 17 || value == 94) { value++; continue }
        out += value.toString() to USER_COUNTRY_ISO[iso]
        value++; iso++
    }
    return out.toTypedArray()
}
