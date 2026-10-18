/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <dlfcn.h>

#include <set>

#include "xenia/gpu/vulkan/vulkan_command_processor.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "xenia/base/assert.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/frame_stats.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/profiling.h"
#include "xenia/base/xxhash.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/gpu_live_options.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_fsi_system_constants.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/vulkan/vulkan_pipeline_cache.h"
#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"
#include "xenia/gpu/vulkan/vulkan_shader.h"
#include "xenia/gpu/vulkan/vulkan_shared_memory.h"
#include "xenia/gpu/vulkan/vulkan_zpd_query_pool.h"
#include "xenia/gpu/xenos.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"
#include "xenia/ui/vulkan/vulkan_instance.h"
#include "xenia/ui/vulkan/vulkan_presenter.h"
#include "xenia/ui/vulkan/vulkan_util.h"

#if defined(__ANDROID__)
DECLARE_int32(adrenotools_turbo_reassert_seconds);
#endif
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(vulkan_in_pass_resolve_debug_read_usage);
DECLARE_bool(log_gpu_frame_time_breakdown);
DECLARE_bool(vulkan_resolve_clear_in_guest_pass);
DECLARE_bool(spirv_specialize_no_alpha);
DECLARE_bool(vulkan_texture_load_coalesced);
DECLARE_bool(vulkan_texture_load_to_image);
DECLARE_bool(vulkan_texture_load_levels_to_image);
DECLARE_int32(vulkan_skip_gpu_written_texture_loads);
DECLARE_bool(readback_resolve_uma_read_watch);
DECLARE_int32(readback_resolve_uma_read_watch_min_kb);
DECLARE_bool(texture_lookup_memo);
DECLARE_bool(texture_partial_reload);
DECLARE_bool(texture_integer_num_format);
DECLARE_bool(vulkan_direct_host_resolve);
DECLARE_bool(vulkan_async_skip_draws);
DECLARE_bool(vulkan_depth_clamp_without_depth);
DECLARE_bool(apu_adaptive_queue);
DECLARE_bool(merge_tiling_bands);
DECLARE_bool(merge_tiling_bands_call_sites);
DECLARE_bool(merge_tiling_bands_log);
DECLARE_bool(vulkan_direct_host_resolve_4px);
DECLARE_bool(vulkan_direct_host_resolve_to_texture);
DECLARE_bool(vulkan_direct_host_resolve_storage_format);
DECLARE_bool(vulkan_direct_host_resolve_7e3_variant);
DECLARE_bool(vulkan_in_pass_resolve_7e3);
DECLARE_bool(vulkan_direct_host_resolve_depth_to_8888);
DECLARE_bool(vulkan_direct_host_resolve_format_variants);
DECLARE_bool(vulkan_resolve_dest_diag);
DECLARE_bool(vulkan_replay_stats);
DECLARE_bool(vulkan_resolve_draw_barriers_at_resolve);
DECLARE_int32(vulkan_debug_gpu_probe);
DECLARE_int32(edram_trace_frames);
DECLARE_bool(skip_overwritten_transfers);
DECLARE_bool(skip_overwritten_transfers_cutout);
DECLARE_bool(transfer_cleared_sources_as_clears);
DECLARE_bool(host_alpha_to_coverage);
DECLARE_bool(memexport_enable);
DECLARE_bool(spirv_texture_sign_specialization);
DECLARE_bool(spirv_texture_sign_specialization_used);
DECLARE_bool(spirv_texture_fetch_constants_decoded);
DECLARE_bool(spirv_texture_exp_adjust_specialization);

DEFINE_bool(
    vulkan_texture_descriptor_set_frame_reuse, true,
    "With the content-hash gate of vulkan_cache_texture_descriptors, also "
    "reuse a texture descriptor set written earlier in the frame with the "
    "same images and samplers, rather than only the one the previous draw "
    "had: the sets live until the frame completes, and draws alternate "
    "between a few texture combinations. Results are identical. Read per draw "
    "(debug.xendroid.texset_reuse on Android).",
    "Vulkan");

DEFINE_bool(
    vulkan_float_constants_full_range, true,
    "With dynamic constant buffers, give the descriptors of the float "
    "constants the range of all 256 (4 KB, what dynamically indexed constants "
    "use already) and keep that much of the upload page after their offset, "
    "rather than the range of the constants the shader reads: the descriptor "
    "set then stays the same when the next shader reads a different number "
    "of them, and only its dynamic offsets change. Shaders read only their "
    "own constants, so the results are identical. Read per draw "
    "(debug.xendroid.const_full_range on Android).",
    "Vulkan");

DEFINE_bool(
    readback_resolve_uma_unread_traffic, false,
    "Diagnostics: when readback_resolve_uma_read_watch skips the copy of a "
    "resolve the CPU hasn't read, still read its bytes out of the GPU's "
    "memory, into a scratch buffer rather than guest memory - the CPU memory "
    "traffic of the copy without its effect on guest memory "
    "(debug.xendroid.uma_traffic on Android).",
    "GPU");

DEFINE_bool(
    vulkan_texture_sign_classes, true,
    "With spirv_texture_sign_specialization: give each draw's pipeline the "
    "signs of the textures its pixel shader fetches (otherwise the shaders "
    "handle them at runtime, as without the option). Read per draw "
    "(debug.xendroid.texture_sign_classes on Android).",
    "GPU");

DEFINE_bool(
    alpha_to_coverage_as_alpha_test, false,
    "Host render target path, trading image quality for speed: draws with "
    "alpha to coverage and no alpha test of their own pass or drop whole "
    "pixels by an alpha test at 0.5 instead (hard edges on foliage and "
    "fences, no partly covered pixels for the multisampled render targets "
    "to blend and store). Can be switched at runtime "
    "(debug.xendroid.a2c_as_test).",
    "GPU");

DEFINE_bool(
    msaa_4x_as_2x, false,
    "Host render target path, trading image quality for speed: store the "
    "guest's 4x MSAA render targets with 2 samples per pixel (2x MSAA "
    "quality). They keep the guest's 4x EDRAM layout - for ownership, "
    "transfers between surfaces and resolves - with each pair of guest "
    "samples of a pixel row sharing one host sample, so titles reading their "
    "4x data as 1x or with another pitch still get it in place; drawing "
    "rasterizes 2 samples. Occlusion query counts are doubled back. Can be "
    "switched at runtime (debug.xendroid.msaa_4x_as_2x).",
    "GPU");

DEFINE_bool(
    vulkan_depth_4x_as_1x, false,
    "Host render target path: render the guest's 4x MSAA depth-only draws into "
    "surfaces never drawn with color - a title's own trick for fast depth "
    "clears and double-resolution depth, like the shadow atlas of Forza "
    "Horizon, read back as a 1x surface - into that 1x surface twice as wide "
    "and tall (the same EDRAM tiles hold the 2x2 samples of a 4x pixel like "
    "2x2 pixels), with the viewport and the scissor doubled, instead of into a "
    "4x render target whose contents have to be transferred to and from the "
    "1x one. Depth is taken at the pixel centers of the double-resolution grid "
    "instead of at the 4x sample positions (up to 1/4 of a 1x pixel away). "
    "Not with resolution scaling. Can be switched at runtime "
    "(debug.xendroid.depth_4x_as_1x).",
    "GPU");

DEFINE_int32(
    vulkan_shading_rate, 0,
    "With vulkan_fragment_shading_rate: shade the draws of multisampled "
    "render targets (the 3D scene, not the 1x post-processing and interface) "
    "once per 0 - pixel, 1 - 2x1 pixels, 2 - 1x2 pixels, 3 - 2x2 pixels. "
    "Coverage, depth and stencil stay per sample: edges keep their "
    "antialiasing, textures and lighting inside triangles get coarser. Can be "
    "switched at runtime (debug.xendroid.shading_rate).",
    "GPU");

DEFINE_bool(
    vulkan_samples_as_pixels_simple_ps, false,
    "With vulkan_depth_4x_as_1x: also render 4x MSAA draws whose pixel shader "
    "reads no textures, gradients or position into the 1x surface of their "
    "samples, and don't count their color as making a depth surface a "
    "multisampled scene's. Titles mark stencil at a quarter of the pixels "
    "this way and switch the same EDRAM between 4x and 1x several times a "
    "frame (Forza Horizon: 4320 of its 8791 transferred tiles a frame). The "
    "pixel shader runs per 1x pixel instead of per 4x pixel - the same "
    "result for flat colors, interpolated values taken at the 1x pixel "
    "centers otherwise. Can be switched at runtime "
    "(debug.xendroid.samples_as_pixels_ps).",
    "GPU");

DEFINE_bool(
    vulkan_samples_as_pixels_2x, false,
    "With vulkan_depth_4x_as_1x: a 4x MSAA draw it would render into the 1x "
    "surface of its samples, over EDRAM a 2x MSAA surface of the same pitch "
    "owns, goes into that 2x surface twice as wide instead (the 2 columns of "
    "samples of a 4x pixel are 2 pixels there, its vertical samples the 2x "
    "ones) - no transfers to the 1x surface and back. Forza Horizon clears "
    "its 256x256 2x reflection with a 4x quad (512 tiles a frame). Can be "
    "switched at runtime (debug.xendroid.samples_as_pixels_2x).",
    "GPU");

DEFINE_bool(
    render_area_dirty_extent, false,
    "Shrink each render pass's area to the region its draws actually touch.\n"
    "Host render targets span the whole EDRAM range for their pitch, so a "
    "narrow one is thousands of rows tall. SHELVED: measured on Adreno 650 / "
    "Turnip this changes nothing - shrinking 80x8192 to 32x32 left pass times "
    "and the RB/CCU/UBWC counters identical, so the driver was already skipping "
    "the empty tiles rather than binning them. Kept for other drivers and as "
    "groundwork; off by default because it buys nothing here.",
    "GPU");

DEFINE_bool(
    log_gpu_frame_time_breakdown_passes, true,
    "With log_gpu_frame_time_breakdown, also time every render pass and every "
    "resolve on the GPU (VkPassTime lines, resolve_ms; VkResolveTime per "
    "resolve kind and size, split at the end of the copy into copy and "
    "clear), and texture loads and the setup command buffer (shared memory "
    "uploads) outside passes (VkMiscTime). Each of those BOTTOM_OF_PIPE "
    "timestamps is a wait-for-idle on Turnip - hundreds per frame - so they "
    "distort what they measure (Forza Horizon on Adreno 825: 7.3 fps "
    "instrumented vs 12.7 fps plain). Disable to keep only the per-submission "
    "GPU busy/gap timestamps and the CPU-side GpuFrame stats, which are cheap "
    "enough to tell a GPU-bound frame from a CPU-bound one.",
    "GPU");

DEFINE_int32(
    vulkan_mid_frame_submission_draws, 1300,
    "If greater than 0, end and submit the current command buffer after this "
    "many draws instead of only at the swap, so the GPU overlaps the frame's "
    "rendering with the building of the rest of its command stream. 0 keeps "
    "one submission per frame. Splitting closes the current render pass, so "
    "values that are too small hurt tiled GPUs; try roughly half the title's "
    "per-frame draw count.",
    "GPU");
UPDATE_from_int32(vulkan_mid_frame_submission_draws, 2026, 7, 24, 12, 0);

DEFINE_int32(
    vulkan_drained_submission_draws, 100,
    "If greater than 0, for the first 16 submissions after the command "
    "processor awaited the GPU until it had nothing left to do (memory export "
    "output the guest reads, the command processor idle), end and submit the "
    "current command buffer every this many draws rather than every "
    "vulkan_mid_frame_submission_draws, so the GPU doesn't idle while the rest "
    "of the command stream is built. Games that never drain the GPU get no "
    "extra submissions. Need for Speed: Most Wanted awaits the GPU 3-4 times "
    "a frame: at the start line of its first race, 11.8-12.1 -> 19.6-19.7 fps "
    "with 100 (50: 18.4-19.2, 200: 15.3, POCO F7, Adreno 825). Read per draw "
    "(debug.xendroid.drained_submit_draws on Android).",
    "GPU");

DEFINE_int32(
    resolve_log_frames, 0,
    "Diagnostics: write one line per resolve of the first this many frames "
    "(source, rectangle, destination, format, written range) to "
    "resolve_log.txt in the storage root - what the GPU wrote where while a "
    "game loads.",
    "GPU");

DEFINE_bool(
    memexport_readback_deferred, true,
    "With memexport_enable on a host-mapped buffer (readback_resolve=uma), "
    "read memory export output back into guest memory once the submissions "
    "of all of it complete, instead of awaiting the GPU after every exporting "
    "draw. While any is pending, a fence, interrupt or coherency request the "
    "guest observes, a wait on a register or memory in its command stream and "
    "the command processor running out of commands await the GPU first (with "
    "memexport_await_fences on or off), so the output is there by then. "
    "Crysis 3 copies memory with exports thousands of times while loading and "
    "stalled on each; Need for Speed: Most Wanted the same either way. On for "
    "every title since 2026-10-07.",
    "GPU");
UPDATE_from_bool(memexport_readback_deferred, 2026, 10, 7, 11, false);

DEFINE_bool(
    vulkan_pixel_shader_vertex_buffers, true,
    "Upload the buffers pixel shaders read with vertex fetch before the draw, "
    "like the vertex shader's. The Xenos fetches vertices in either stage: "
    "Rise of the Tomb Raider's pixel shaders that sum the morph targets of "
    "Lara's clothes read their deltas so, and with only the vertex shader's "
    "buffers uploaded they read zeros (her clothes stretched into a plank). "
    "Off is the old behavior, for comparison. "
    "debug.xendroid.ps_vertex_buffers switches it at run time on Android "
    "(read per draw; a buffer uploaded stays until the CPU writes it).",
    "Vulkan");

DEFINE_bool(
    vulkan_cache_texture_descriptors, true,
    "Skip re-writing and re-binding the texture/sampler descriptor sets on "
    "draws whose resolved image views and samplers have not changed since the "
    "last write. Disable to force a descriptor-set write and bind every draw "
    "(pre-optimization behavior) for debugging texture corruption.",
    "Vulkan");

// A/B selector for the cross-draw texture/sampler descriptor-set reuse gate.
// Default (false) keeps XenDroid's content-hash gate; true switches to upstream
// edge's bitmask+shader-pointer+sampler-vector gate. Only consulted when
// vulkan_cache_texture_descriptors is on. Both schemes coexist so the cvar can
// be flipped live for comparison (see the gate and write-back in UpdateBindings
// for the switch-safety invariants).
DEFINE_bool(
    vulkan_texture_descriptor_reuse_edge, false,
    "When vulkan_cache_texture_descriptors is on, use upstream edge's "
    "bitmask+shader-pointer+sampler-vector descriptor-set reuse gate instead of "
    "XenDroid's content-hash gate. For A/B comparison; default false = the hash "
    "gate.",
    "Vulkan");

DEFINE_bool(
    vulkan_skip_redundant_fetch_constant_writes, true,
    "Don't invalidate texture bindings and the fetch/bool-loop constant "
    "buffers when a register write doesn't change the register's value. "
    "Games commonly re-emit identical fetch constants every draw, and the "
    "derived state is a pure function of the register contents.",
    "Vulkan");

DEFINE_bool(
    vulkan_cache_sampler_parameters, true,
    "Reuse sampler parameters and VkSampler handles across draws, re-deriving "
    "them only for fetch constants written since the previous draw (or on "
    "shader sampler-layout change, new submission, or sampler destruction). "
    "Disable to derive and look up every sampler on every draw "
    "(pre-optimization behavior).",
    "Vulkan");

DEFINE_bool(
    vulkan_fast_register_ranges, true,
    "Process PM4 register range writes with bulk byte-swapped copies and "
    "range-level constant dirty tracking (port of the D3D12 backend's "
    "register-write fast path) instead of a virtual per-register call. "
    "Disable to route every register through WriteRegister "
    "(pre-optimization behavior).",
    "Vulkan");

DEFINE_bool(
    vulkan_allow_reverse_z, true,
    "Pass viewports with minDepth > maxDepth (reverse depth ranges, used by "
    "many games for inverse-Z) directly to the driver. Vulkan allows this, "
    "but some drivers mishandle it (the Direct3D 12 backend always "
    "normalizes, citing drivers where it works and drivers where it "
    "doesn't). Disable to normalize the range and compensate in the NDC "
    "transform like the D3D12 backend does.",
    "Vulkan");

DEFINE_bool(
    vulkan_dynamic_constant_buffers, false,
    "Bind the guest constant buffers as VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC "
    "so only the per-draw dynamic offsets vary; the constants descriptor set is "
    "re-allocated and re-written only when an upload-pool page rolls over. "
    "DEFAULT OFF: on Qualcomm proprietary Vulkan drivers (Adreno 650/740/830 "
    "tested, 2026-06) shaders read ZEROS through the dynamic-offset uniform "
    "descriptors, breaking all rendering (black output / garbage), proven by "
    "RenderDoc pixel-history bisection; Mesa/Turnip reads them correctly. "
    "Safe to enable on Turnip for a small command-processor win. Has no "
    "effect on devices that report maxDescriptorSetUniformBuffersDynamic < 7 "
    "(the guest constant-buffer count).",
    "Vulkan");

DEFINE_bool(
    vulkan_dynamic_constant_buffers_turnip, true,
    "vulkan_dynamic_constant_buffers on the Mesa Turnip driver, which reads "
    "dynamic uniform buffers correctly (the proprietary Qualcomm drivers the "
    "option stays off for don't). Red Dead Redemption otherwise writes a "
    "constants descriptor set for nearly every draw (7260 a frame). Read at "
    "startup.",
    "Vulkan");

DEFINE_int32(
    vulkan_debug_extra_pass_breaks, 0,
    "Diagnostics - measures what a render pass break costs the GPU: end the "
    "guest render pass before every Nth draw (0 = never). A negative N ends it "
    "with a shared memory barrier between the guest shader stages, like the "
    "breaks that pending barriers cause. Read per draw "
    "(debug.xendroid.extra_pass_breaks on Android).",
    "Vulkan");

DEFINE_int32(
    vulkan_debug_draw_ceiling, 0,
    "Diagnostics (breaks the image) - what the parts of the guest draws cost "
    "the GPU, with the per-pass timestamps: 1 - rasterize nothing (a 1x1 "
    "scissor), keeping the vertex work, the state and the passes; 2 - also "
    "only the first primitive of each draw; 3 - no draw commands at all, "
    "keeping everything around them. Read per draw (debug.xendroid.draw_ceiling "
    "on Android).",
    "Vulkan");

DEFINE_int32(
    vulkan_log_slow_submission_ms, 15,
    "With log_gpu_frame_time_breakdown: log a VkSlowSubmission line for each "
    "submission whose GPU time passes this many milliseconds, with the work "
    "it carried (draws, resolves, texture loads and their texels, shared "
    "memory upload bytes) - what the rare long frames are made of. 0 "
    "disables.",
    "Vulkan");

DECLARE_bool(gpu_debug_markers);
DECLARE_bool(submit_on_primary_buffer_end);
DECLARE_bool(vulkan_placeholder_pipelines);

DEFINE_bool(
    vulkan_dynamic_rendering, true,
    "Use VK_KHR_dynamic_rendering instead of traditional render passes. "
    "May improve or worsen performance depending on driver. Requires Vulkan "
    "1.3 or VK_KHR_dynamic_rendering extension support.",
    "Vulkan");

namespace {
// Where the awaits for the GPU that blocked came from, with
// log_gpu_frame_time_breakdown: the return addresses into the callers of
// CheckSubmissionCompletionAndDeviceLoss (offsets in the library for
// addr2line), with their count and time, reported as VkAwaitSites.
struct AwaitSite {
  uintptr_t address;
  uint64_t count;
  uint64_t ns;
};
AwaitSite await_sites[8];
// The same for SubmitBarriers calls that recorded barriers (VkBarrierSites):
// the callers' return addresses, the pipeline barriers recorded, and the
// stage masks of those barriers.
struct BarrierSite {
  uintptr_t address;
  uint64_t calls;
  uint64_t barriers;
};
BarrierSite barrier_sites[16];
struct BarrierStages {
  uint32_t src;
  uint32_t dst;
  uint64_t count;
};
BarrierStages barrier_stages[12];
}  // namespace

namespace xe {
namespace gpu {
namespace vulkan {

namespace {

#if defined(__ANDROID__)
void PollDebugPropertyOverride(const char* property, const char* cvar_name,
                               bool& cvar) {
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get(property, value) > 0 &&
      (value[0] == '0' || value[0] == '1')) {
    const bool enabled = value[0] == '1';
    if (cvar != enabled) {
      cvar = enabled;
      XELOGI("{}: {} = {}", property, cvar_name, enabled);
    }
  }
}
#endif

// The in-game menu's GPU options (gpu_live_options.h), every frame: they
// change only when the player picks another value.
void ApplyLiveOptions() {
  auto apply_bool = [](LiveOption option, const char* cvar_name, bool& cvar) {
    const int32_t value =
        LiveOptionValue(option).load(std::memory_order_relaxed);
    if (value == kLiveOptionUnset || cvar == (value != 0)) {
      return;
    }
    cvar = value != 0;
    XELOGI("In-game menu: {} = {}", cvar_name, cvar);
  };
  apply_bool(LiveOption::kAsyncSkipDraws, "vulkan_async_skip_draws",
             cvars::vulkan_async_skip_draws);
  apply_bool(LiveOption::kMsaa4xAs2x, "msaa_4x_as_2x", cvars::msaa_4x_as_2x);
  apply_bool(LiveOption::kAlphaToCoverageAsAlphaTest,
             "alpha_to_coverage_as_alpha_test",
             cvars::alpha_to_coverage_as_alpha_test);
  const int32_t shading_rate =
      LiveOptionValue(LiveOption::kShadingRate).load(std::memory_order_relaxed);
  if (shading_rate >= 0 && shading_rate <= 3 &&
      cvars::vulkan_shading_rate != shading_rate) {
    cvars::vulkan_shading_rate = shading_rate;
    XELOGI("In-game menu: vulkan_shading_rate = {}", shading_rate);
  }
}

// On-device A/B switches that need no title restart (the per-game config is
// only applied at launch), e.g. `adb shell setprop
// debug.xendroid.resolve_clear_in_guest_pass 0|1`. An empty value stops the
// override, but the last forced value stays until the title is relaunched,
// and a property left set (they last until reboot) overrides every later
// launch 30 frames in - tools/fh_auto.sh clears them before launching.
// Polled every 30 guest frames from the command processor thread, which is
// the only reader of these cvars but apu_adaptive_queue (the Audio Worker's,
// a plain bool read once a pump).
void PollDebugPropertyOverrides(CommandProcessor& command_processor) {
  ApplyLiveOptions();
#if defined(__ANDROID__)
  static uint32_t frames_since_poll = 0;
  if (++frames_since_poll < 30) {
    return;
  }
  frames_since_poll = 0;
  PollDebugPropertyOverride("debug.xendroid.resolve_clear_in_guest_pass",
                            "vulkan_resolve_clear_in_guest_pass",
                            cvars::vulkan_resolve_clear_in_guest_pass);
  // Read per frame (merge_tiling_bands) and per draw (the other two).
  PollDebugPropertyOverride("debug.xendroid.merge_bands", "merge_tiling_bands",
                            cvars::merge_tiling_bands);
  PollDebugPropertyOverride("debug.xendroid.band_call_sites",
                            "merge_tiling_bands_call_sites",
                            cvars::merge_tiling_bands_call_sites);
  PollDebugPropertyOverride("debug.xendroid.band_log", "merge_tiling_bands_log",
                            cvars::merge_tiling_bands_log);
  // Selects the pixel shader variant per draw, so switching it creates the
  // other variant's pipelines on the fly (async, like a new area would).
  PollDebugPropertyOverride("debug.xendroid.spirv_specialize_no_alpha",
                            "spirv_specialize_no_alpha",
                            cvars::spirv_specialize_no_alpha);
  // Read per texture load; both pipelines are created at startup.
  PollDebugPropertyOverride("debug.xendroid.texload_coalesced",
                            "vulkan_texture_load_coalesced",
                            cvars::vulkan_texture_load_coalesced);
  // Read per texture load; only textures created while it was set at startup
  // have the storage alias it needs.
  PollDebugPropertyOverride("debug.xendroid.texload_to_image",
                            "vulkan_texture_load_to_image",
                            cvars::vulkan_texture_load_to_image);
  PollDebugPropertyOverride("debug.xendroid.uma_read_watch",
                            "readback_resolve_uma_read_watch",
                            cvars::readback_resolve_uma_read_watch);
  PollDebugPropertyOverride("debug.xendroid.partial_reload",
                            "texture_partial_reload",
                            cvars::texture_partial_reload);
  PollDebugPropertyOverride("debug.xendroid.lookup_memo", "texture_lookup_memo",
                            cvars::texture_lookup_memo);
  PollDebugPropertyOverride("debug.xendroid.texset_reuse",
                            "vulkan_texture_descriptor_set_frame_reuse",
                            cvars::vulkan_texture_descriptor_set_frame_reuse);
  PollDebugPropertyOverride("debug.xendroid.const_full_range",
                            "vulkan_float_constants_full_range",
                            cvars::vulkan_float_constants_full_range);
  PollDebugPropertyOverride("debug.xendroid.uma_traffic",
                            "readback_resolve_uma_unread_traffic",
                            cvars::readback_resolve_uma_unread_traffic);
  PollDebugPropertyOverride("debug.xendroid.texload_levels",
                            "vulkan_texture_load_levels_to_image",
                            cvars::vulkan_texture_load_levels_to_image);
  // Read per resolve (0: dump the render targets to the EDRAM buffer and copy
  // from there).
  PollDebugPropertyOverride("debug.xendroid.direct_host_resolve",
                            "vulkan_direct_host_resolve",
                            cvars::vulkan_direct_host_resolve);
  // Read per resolve; the pipelines of both are created on first use.
  PollDebugPropertyOverride("debug.xendroid.resolve_4px",
                            "vulkan_direct_host_resolve_4px",
                            cvars::vulkan_direct_host_resolve_4px);
  // Read per resolve; textures are promoted (created with the storage alias)
  // only from destinations recorded while it is on.
  PollDebugPropertyOverride("debug.xendroid.resolve_to_texture",
                            "vulkan_direct_host_resolve_to_texture",
                            cvars::vulkan_direct_host_resolve_to_texture);
  PollDebugPropertyOverride("debug.xendroid.resolve_storage_format",
                            "vulkan_direct_host_resolve_storage_format",
                            cvars::vulkan_direct_host_resolve_storage_format);
  PollDebugPropertyOverride("debug.xendroid.resolve_7e3_variant",
                            "vulkan_direct_host_resolve_7e3_variant",
                            cvars::vulkan_direct_host_resolve_7e3_variant);
  PollDebugPropertyOverride("debug.xendroid.in_pass_resolve_7e3",
                            "vulkan_in_pass_resolve_7e3",
                            cvars::vulkan_in_pass_resolve_7e3);
  PollDebugPropertyOverride("debug.xendroid.resolve_depth_to_8888",
                            "vulkan_direct_host_resolve_depth_to_8888",
                            cvars::vulkan_direct_host_resolve_depth_to_8888);
  PollDebugPropertyOverride("debug.xendroid.resolve_format_variants",
                            "vulkan_direct_host_resolve_format_variants",
                            cvars::vulkan_direct_host_resolve_format_variants);
  PollDebugPropertyOverride("debug.xendroid.resolve_dest_diag",
                            "vulkan_resolve_dest_diag",
                            cvars::vulkan_resolve_dest_diag);
  PollDebugPropertyOverride("debug.xendroid.replay_stats",
                            "vulkan_replay_stats", cvars::vulkan_replay_stats);
  // Read per draw and resolve; the render target cache moves the contents
  // between the 4x and the 2x render targets like for any other key change.
  PollDebugPropertyOverride("debug.xendroid.msaa_4x_as_2x", "msaa_4x_as_2x",
                            cvars::msaa_4x_as_2x);
  // Read per draw: the pipelines lose or regain the host's alpha to coverage.
  PollDebugPropertyOverride("debug.xendroid.a2c_as_test",
                            "alpha_to_coverage_as_alpha_test",
                            cvars::alpha_to_coverage_as_alpha_test);
  // Read per draw (pipeline descriptions): new pipelines on the fly.
  PollDebugPropertyOverride("debug.xendroid.depth_clamp_no_depth",
                            "vulkan_depth_clamp_without_depth",
                            cvars::vulkan_depth_clamp_without_depth);
  // Read per draw.
  PollDebugPropertyOverride("debug.xendroid.ps_vertex_buffers",
                            "vulkan_pixel_shader_vertex_buffers",
                            cvars::vulkan_pixel_shader_vertex_buffers);
  // Read per audio pump.
  PollDebugPropertyOverride("debug.xendroid.apu_adaptive_queue",
                            "apu_adaptive_queue", cvars::apu_adaptive_queue);
  // Read per draw, like msaa_4x_as_2x.
  PollDebugPropertyOverride("debug.xendroid.depth_4x_as_1x",
                            "vulkan_depth_4x_as_1x",
                            cvars::vulkan_depth_4x_as_1x);
  // Read per draw; the surfaces classified as multisampled scenes are
  // forgotten when it changes.
  PollDebugPropertyOverride("debug.xendroid.samples_as_pixels_ps",
                            "vulkan_samples_as_pixels_simple_ps",
                            cvars::vulkan_samples_as_pixels_simple_ps);
  PollDebugPropertyOverride("debug.xendroid.samples_as_pixels_2x",
                            "vulkan_samples_as_pixels_2x",
                            cvars::vulkan_samples_as_pixels_2x);
  // Read per resolve.
  PollDebugPropertyOverride("debug.xendroid.resolve_draw_barriers",
                            "vulkan_resolve_draw_barriers_at_resolve",
                            cvars::vulkan_resolve_draw_barriers_at_resolve);
  // Read per draw.
  PollDebugPropertyOverride("debug.xendroid.skip_overwritten_transfers",
                            "skip_overwritten_transfers",
                            cvars::skip_overwritten_transfers);
  PollDebugPropertyOverride("debug.xendroid.transfer_cutout",
                            "skip_overwritten_transfers_cutout",
                            cvars::skip_overwritten_transfers_cutout);
  PollDebugPropertyOverride("debug.xendroid.cleared_transfers",
                            "transfer_cleared_sources_as_clears",
                            cvars::transfer_cleared_sources_as_clears);
  PollDebugPropertyOverride("debug.xendroid.texture_sign_classes",
                            "vulkan_texture_sign_classes",
                            cvars::vulkan_texture_sign_classes);
  // Read per texture binding; the shaders have the rescale only if it was on
  // when they were translated (launch with it on to compare both ways).
  PollDebugPropertyOverride("debug.xendroid.texture_integer_num_format",
                            "texture_integer_num_format",
                            cvars::texture_integer_num_format);
  // Read per exporting draw; one read back right away first reads back those
  // still pending.
  PollDebugPropertyOverride("debug.xendroid.memexport_deferred",
                            "memexport_readback_deferred",
                            cvars::memexport_readback_deferred);
  // Both texture load switches at once, for A/Bs of the load paths: 0 - the
  // original untiling into a buffer copied to the image, 1 - coalesced
  // untiling, 2 - coalesced straight into the image (which only has the
  // storage aliases it needs if it was on at startup).
  char texload_mode_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.texload_mode",
                            texload_mode_value) > 0 &&
      texload_mode_value[0] >= '0' && texload_mode_value[0] <= '2' &&
      !texload_mode_value[1]) {
    const int mode = texload_mode_value[0] - '0';
    if (cvars::vulkan_texture_load_coalesced != (mode >= 1) ||
        cvars::vulkan_texture_load_to_image != (mode >= 2)) {
      cvars::vulkan_texture_load_coalesced = mode >= 1;
      cvars::vulkan_texture_load_to_image = mode >= 2;
      XELOGI("debug.xendroid.texload_mode: texload_mode = {}", mode);
    }
  }
  // Diagnostics: skip the loads of cube maps (1) or GPU-written textures (2).
  char skip_texload_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.skip_texload",
                            skip_texload_value) > 0 &&
      skip_texload_value[0] >= '0' && skip_texload_value[0] <= '2' &&
      !skip_texload_value[1]) {
    const int32_t mode = skip_texload_value[0] - '0';
    if (cvars::vulkan_skip_gpu_written_texture_loads != mode) {
      cvars::vulkan_skip_gpu_written_texture_loads = mode;
      XELOGI(
          "debug.xendroid.skip_texload: "
          "vulkan_skip_gpu_written_texture_loads = {}",
          mode);
    }
  }
  // Resolves below this size copied whether read or not, read per resolve.
  char uma_watch_min_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.uma_watch_min_kb",
                            uma_watch_min_value) > 0 &&
      uma_watch_min_value[0] >= '0' && uma_watch_min_value[0] <= '9') {
    const int32_t min_kb = std::atoi(uma_watch_min_value);
    if (cvars::readback_resolve_uma_read_watch_min_kb != min_kb) {
      cvars::readback_resolve_uma_read_watch_min_kb = min_kb;
      XELOGI(
          "debug.xendroid.uma_watch_min_kb: "
          "readback_resolve_uma_read_watch_min_kb = {}",
          min_kb);
    }
  }
  // The shading rate of the multisampled scene draws, read per draw.
  char shading_rate_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.shading_rate",
                            shading_rate_value) > 0 &&
      shading_rate_value[0] >= '0' && shading_rate_value[0] <= '3' &&
      !shading_rate_value[1]) {
    const int32_t rate = shading_rate_value[0] - '0';
    if (cvars::vulkan_shading_rate != rate) {
      cvars::vulkan_shading_rate = rate;
      XELOGI("debug.xendroid.shading_rate: vulkan_shading_rate = {}", rate);
    }
  }
  // Draws per mid-frame submission (0 = one submission per frame), read per
  // draw.
  char submit_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.submit_draws", submit_value) > 0 &&
      submit_value[0] >= '0' && submit_value[0] <= '9') {
    const int32_t draws = std::atoi(submit_value);
    if (cvars::vulkan_mid_frame_submission_draws != draws) {
      cvars::vulkan_mid_frame_submission_draws = draws;
      XELOGI(
          "debug.xendroid.submit_draws: vulkan_mid_frame_submission_draws = {}",
          draws);
    }
  }
  // Draws per submission right after the GPU was drained (0 = off), read per
  // draw.
  char drained_submit_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.drained_submit_draws",
                            drained_submit_value) > 0 &&
      drained_submit_value[0] >= '0' && drained_submit_value[0] <= '9') {
    const int32_t draws = std::atoi(drained_submit_value);
    if (cvars::vulkan_drained_submission_draws != draws) {
      cvars::vulkan_drained_submission_draws = draws;
      XELOGI(
          "debug.xendroid.drained_submit_draws: vulkan_drained_submission_draws "
          "= {}",
          draws);
    }
  }
  // none | uma | fast | all, read per resolve.
  char readback_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.readback_resolve",
                            readback_value) > 0) {
    static const std::pair<const char*, ReadbackResolveMode> kModes[] = {
        {"none", ReadbackResolveMode::kDisabled},
        {"uma", ReadbackResolveMode::kUma},
        {"fast", ReadbackResolveMode::kFast},
        {"all", ReadbackResolveMode::kAll},
    };
    for (const auto& mode : kModes) {
      if (!std::strcmp(readback_value, mode.first)) {
        if (command_processor.GetReadbackResolveMode() != mode.second) {
          // A session override - never written to the per-game config.
          command_processor.SetReadbackResolveMode(mode.second, false);
          XELOGI("debug.xendroid.readback_resolve: readback_resolve = {}",
                 mode.first);
        }
        break;
      }
    }
  }
  // Seconds between KGSL power control requests (0: only at startup), read
  // per swap.
  char reassert_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.turbo_reassert", reassert_value) >
          0 &&
      reassert_value[0] >= '0' && reassert_value[0] <= '9') {
    const int32_t seconds = std::atoi(reassert_value);
    if (cvars::adrenotools_turbo_reassert_seconds != seconds) {
      cvars::adrenotools_turbo_reassert_seconds = seconds;
      XELOGI(
          "debug.xendroid.turbo_reassert: adrenotools_turbo_reassert_seconds "
          "= {}",
          seconds);
    }
  }
  // Extra render pass breaks every N draws (negative: with a barrier), read
  // per draw.
  char breaks_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.extra_pass_breaks",
                            breaks_value) > 0 &&
      (breaks_value[0] == '-' ||
       (breaks_value[0] >= '0' && breaks_value[0] <= '9'))) {
    const int32_t every = std::atoi(breaks_value);
    if (cvars::vulkan_debug_extra_pass_breaks != every) {
      cvars::vulkan_debug_extra_pass_breaks = every;
      XELOGI(
          "debug.xendroid.extra_pass_breaks: vulkan_debug_extra_pass_breaks = "
          "{}",
          every);
    }
  }
  // What the parts of the draws cost, read per draw.
  char ceiling_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.draw_ceiling", ceiling_value) > 0 &&
      ceiling_value[0] >= '0' && ceiling_value[0] <= '3') {
    const int32_t mode = ceiling_value[0] - '0';
    if (cvars::vulkan_debug_draw_ceiling != mode) {
      cvars::vulkan_debug_draw_ceiling = mode;
      XELOGI("debug.xendroid.draw_ceiling: vulkan_debug_draw_ceiling = {}",
             mode);
    }
  }
  // EDRAM usage trace of the next N frames, a new value starting a new trace.
  char edram_trace_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.edram_trace", edram_trace_value) >
          0 &&
      edram_trace_value[0] >= '0' && edram_trace_value[0] <= '9') {
    const int32_t frames = std::atoi(edram_trace_value);
    if (cvars::edram_trace_frames != frames) {
      cvars::edram_trace_frames = frames;
      XELOGI("debug.xendroid.edram_trace: edram_trace_frames = {}", frames);
    }
  }
  // The GPU work skipped to measure what it costs, read per resolve / draw.
  char probe_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.gpu_probe", probe_value) > 0 &&
      probe_value[0] >= '0' && probe_value[0] <= '9') {
    const int32_t probe = std::atoi(probe_value);
    if (cvars::vulkan_debug_gpu_probe != probe) {
      cvars::vulkan_debug_gpu_probe = probe;
      XELOGI("debug.xendroid.gpu_probe: vulkan_debug_gpu_probe = {}", probe);
    }
  }
  // Bounded fence collection (all Vulkan completion timelines), read per poll.
  char fence_value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.xendroid.fence_collect", fence_value) > 0 &&
      (fence_value[0] == '0' || fence_value[0] == '1')) {
    const bool bounded = fence_value[0] == '1';
    if (ui::vulkan::VulkanGPUCompletionTimeline::bounded_collection() !=
        bounded) {
      ui::vulkan::VulkanGPUCompletionTimeline::SetBoundedCollection(bounded);
      XELOGI("debug.xendroid.fence_collect: bounded fence collection = {}",
             bounded);
    }
  }
#endif
}

}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/fxaa_extreme_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        SpirvShaderTranslator::kConstantBufferCount*
            kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kLinkedTypeDescriptorPoolSetCount};

constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeStorageImage = {
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
constexpr VkDescriptorPoolSize
    VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         2 * kLinkedTypeDescriptorPoolSetCount},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(
    VulkanGraphicsSystem* graphics_system, kernel::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      completion_timeline_(static_cast<const ui::vulkan::VulkanProvider*>(
                               graphics_system->provider())
                               ->vulkan_device(),
                           "cp"),
      deferred_command_buffer_(*this),
      deferred_setup_command_buffer_(*this, 64 * 1024),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_image_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageImage, 1,
          kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(
              graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures,
          uint32_t(xe::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::UpdateDebugMarkersEnabled() {
  // Enable debug markers if the CVAR is set or RenderDoc is detected.
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void VulkanCommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_.CmdVkBeginDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_buffer_.CmdVkEndDebugUtilsLabelEXT();
}

void VulkanCommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_buffer_.CmdVkInsertDebugUtilsLabelEXT(label);
}

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  shared_memory_->InvalidateAllPages();
  // No watch fires for this, so the resolve-to-texture store tracking would
  // keep images that the reloaded guest memory no longer matches.
  if (texture_cache_) {
    texture_cache_->InvalidateResolveStoreTracking();
  }
}

void VulkanCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr,
                                                      uint32_t length) {
  shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  primitive_processor_->MemoryInvalidationCallback(base_ptr, length, true);
}

void VulkanCommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking,
                                            nullptr);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking,
                                           std::move(completion_callback));
}

void VulkanCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {}

void VulkanCommandProcessor::PrepareForWait() {
  // Out of commands, or the guest's command stream waiting: export output read
  // back once its submission completes would otherwise wait for a submission
  // nothing makes, while the guest may be waiting for it to go on.
  AwaitMemexportReadbacks("idle or register wait");
  CommandProcessor::PrepareForWait();
}

void VulkanCommandProcessor::PollCompletedSubmission() {
  // Strict ZPD can skip unnecessary work here that can wait for the next full
  // CheckSubmissionCompletionAndDeviceLoss and it's not needed for retirement.
  if (device_lost_) {
    return;
  }
  completion_timeline_.AwaitSubmissionAndUpdateCompleted(
      GetCompletedSubmission());
  PumpQueryResolves();
}

std::string VulkanCommandProcessor::GetTitleStateSuffix() const {
  if (!render_target_cache_) {
    return {};
  }
  std::ostringstream suffix;
  switch (render_target_cache_->GetPath()) {
    case RenderTargetCache::Path::kHostRenderTargets:
      suffix << " - FBO";
      break;
    case RenderTargetCache::Path::kPixelShaderInterlock:
      suffix << " - FSI";
      break;
    default:
      break;
  }
  uint32_t draw_resolution_scale_x =
      texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
  uint32_t draw_resolution_scale_y =
      texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    suffix << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
  }
  return suffix.str();
}

bool VulkanCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    XELOGE("Failed to initialize base command processor context");
    return false;
  }

  // Check if debug markers should be enabled (CVAR or RenderDoc detection).
  UpdateDebugMarkersEnabled();
  if (debug_markers_enabled_) {
    XELOGI("GPU debug markers enabled for RenderDoc/debug tools");
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      xe::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize,
                         size_t(16384)),
                size_t(device_properties.minUniformBufferOffsetAlignment)));

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_empty_) != VK_SUCCESS) {
    XELOGE("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  // Dynamic constant buffers capability gate, resolved once before the constants layout is
  // created and held constant for the device lifetime. The Vulkan-guaranteed
  // minimum for maxDescriptorSetUniformBuffersDynamic is 8, so this passes on
  // all conformant hardware; if it ever fails, the binding stays plain
  // UNIFORM_BUFFER and the original per-draw write+bind path runs unchanged.
  use_dynamic_constants_ =
      (cvars::vulkan_dynamic_constant_buffers ||
       (cvars::vulkan_dynamic_constant_buffers_turnip &&
        device_properties.driverID == VK_DRIVER_ID_MESA_TURNIP)) &&
      device_properties.maxDescriptorSetUniformBuffersDynamic >=
          SpirvShaderTranslator::kConstantBufferCount;
  XELOGI("Vulkan: constant buffers bound as {} uniform buffers",
         use_dynamic_constants_ ? "dynamic" : "plain");
  const VkDescriptorType constants_descriptor_type =
      use_dynamic_constants_ ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding =
        descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = constants_descriptor_type;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferSystem]
          .stageFlags =
      // Visible to the vertex/domain stages for user clip planes and to the
      // control/eval/vertex stages for tessellation, plus the usual guest
      // stages.
      guest_shader_stages | guest_shader_vertex_stages_ |
      (device_properties.tessellationShader
           ? (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)
           : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatVertex]
          .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFloatPixel]
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferBoolLoop]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants
      [SpirvShaderTranslator::kConstantBufferFetch]
          .stageFlags = guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(xe::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_constants_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: storage buffer for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings =
      &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferFragment)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the fragment shader");
    return false;
  }
  // Transient: storage image for the resolve-to-texture fragment variant.
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageImageFragment)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage image "
        "bound to the fragment shader");
    return false;
  }
  descriptor_set_layout_binding_transient.stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageImageCompute)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a storage image "
        "bound to the compute shader");
    return false;
  }
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.binding = 1;
  descriptor_set_layout_binding_transient.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kUniformBufferComputeB1)]) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for a uniform buffer "
        "bound to binding 1 in the compute shader");
    return false;
  }

  shared_memory_ = std::make_unique<VulkanSharedMemory>(
      *this, *memory_, trace_writer_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    XELOGE("Failed to initialize shared memory");
    return false;
  }

  // Read-watch consumption tracking for resolves.
  InitResolveReadWatch();
  resolve_read_callback_ = memory_->RegisterPhysicalMemoryReadCallback(
      ResolveReadCallbackThunk, this);

  primitive_processor_ = std::make_unique<VulkanPrimitiveProcessor>(
      *register_file_, *memory_, trace_writer_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    XELOGE("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1)
                                         << shared_memory_binding_count_log2;

  // Requires the transient descriptor set layouts.
  // Get draw resolution scale and clamp based on device capabilities
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x,
                                                 draw_resolution_scale_y);
  // Check if sparse binding is supported for resolution scaling
  bool has_sparse_binding = device_properties.sparseBinding &&
                            device_properties.sparseResidencyBuffer;
  if (!TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, has_sparse_binding,
          0)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    XELOGW(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, trace_writer_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    XELOGE("Failed to initialize the render target cache");
    return false;
  }

  // Shared memory, EDRAM, and ZPD FSI counter descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  VkDescriptorSetLayoutBinding
      shared_memory_and_edram_descriptor_set_layout_bindings[3];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags =
      guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers =
      nullptr;
  VkDescriptorSetLayoutCreateInfo
      shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount =
        1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1]
        .pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
    // ZPD FSI counter.
    shared_memory_and_edram_descriptor_set_layout_bindings[2].binding = 2;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].descriptorCount =
        1;
    shared_memory_and_edram_descriptor_set_layout_bindings[2].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[2]
        .pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 3;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info,
          nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_,
      guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    XELOGE("Failed to initialize the graphics pipeline cache");
    return false;
  }

  // Requires the transient descriptor set layouts.
  // Use the same draw resolution scale as render target cache
  texture_cache_ = VulkanTextureCache::Create(
      *register_file_, *shared_memory_, draw_resolution_scale_x,
      draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    XELOGE("Failed to initialize the texture cache");
    return false;
  }

  // Fallback for query segment normalization when no draw pinned a scale.
  zpd_draw_resolution_scale_x_ = draw_resolution_scale_x;
  zpd_draw_resolution_scale_y_ = draw_resolution_scale_y;

  const VkDeviceSize zpd_fsi_counter_sink_range =
      sizeof(uint32_t) * kZPDQueryPoolCapacity;
  if (edram_fragment_shader_interlock) {
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, zpd_fsi_counter_sink_range,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            zpd_fsi_counter_sink_buffer_,
            zpd_fsi_counter_sink_buffer_memory_)) {
      XELOGE("Failed to create the ZPD FSI counter sink buffer");
      return false;
    }
  }

  // Shared memory, EDRAM, and ZPD FSI counter common bindings. A second set
  // (binding 0 -> host-imported buffer) is allocated for memexport routing when
  // the host buffer is available.
  const bool create_host_shared_memory_set =
      shared_memory_->host_buffer() != VK_NULL_HANDLE;
  const uint32_t shared_memory_set_count =
      create_host_shared_memory_set ? 2u : 1u;
  VkDescriptorPoolSize descriptor_pool_sizes[1];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_set_count *
      (shared_memory_binding_count +
       2u * uint32_t(edram_fragment_shader_interlock));
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = shared_memory_set_count;
  descriptor_pool_create_info.poolSizeCount = 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool =
      shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) !=
      VK_SUCCESS) {
    XELOGE(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  if (create_host_shared_memory_set) {
    if (dfn.vkAllocateDescriptorSets(
            device, &descriptor_set_allocate_info,
            &shared_memory_host_and_edram_descriptor_set_) != VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the host-imported Vulkan descriptor set for "
          "shared memory memexport routing");
      return false;
    }
  }
  // Writes binding 0 (shared memory, split across shared_memory_binding_count
  // sub-ranges) and, under FSI, binding 1 (EDRAM) and binding 2 (ZPD FSI
  // counter placeholder) of one shared-memory/EDRAM set. shared_memory_buffer
  // selects the device-local buffer or the host-imported buffer.
  auto write_shared_memory_and_edram_set = [&](VkDescriptorSet set,
                                               VkBuffer shared_memory_buffer) {
    VkDescriptorBufferInfo
        shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize /
                                              (128 << 20)];
    uint32_t shared_memory_binding_range =
        SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
    for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
      VkDescriptorBufferInfo& info = shared_memory_descriptor_buffers_info[i];
      info.buffer = shared_memory_buffer;
      info.offset = shared_memory_binding_range * i;
      info.range = shared_memory_binding_range;
    }
    VkWriteDescriptorSet write_descriptor_sets[3];
    VkWriteDescriptorSet& write_shared_memory = write_descriptor_sets[0];
    write_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_shared_memory.pNext = nullptr;
    write_shared_memory.dstSet = set;
    write_shared_memory.dstBinding = 0;
    write_shared_memory.dstArrayElement = 0;
    write_shared_memory.descriptorCount = shared_memory_binding_count;
    write_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_shared_memory.pImageInfo = nullptr;
    write_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
    write_shared_memory.pTexelBufferView = nullptr;
    VkDescriptorBufferInfo edram_descriptor_buffer_info;
    VkDescriptorBufferInfo zpd_fsi_counter_descriptor_buffer_info;
    if (edram_fragment_shader_interlock) {
      edram_descriptor_buffer_info.buffer =
          render_target_cache_->edram_buffer();
      edram_descriptor_buffer_info.offset = 0;
      edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
      VkWriteDescriptorSet& write_edram = write_descriptor_sets[1];
      write_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_edram.pNext = nullptr;
      write_edram.dstSet = set;
      write_edram.dstBinding = 1;
      write_edram.dstArrayElement = 0;
      write_edram.descriptorCount = 1;
      write_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_edram.pImageInfo = nullptr;
      write_edram.pBufferInfo = &edram_descriptor_buffer_info;
      write_edram.pTexelBufferView = nullptr;
      // ZPD FSI counter - kept valid until the real counter buffer is ready.
      zpd_fsi_counter_descriptor_buffer_info.buffer =
          zpd_fsi_counter_sink_buffer_;
      zpd_fsi_counter_descriptor_buffer_info.offset = 0;
      zpd_fsi_counter_descriptor_buffer_info.range = zpd_fsi_counter_sink_range;
      VkWriteDescriptorSet& write_zpd_fsi_counter = write_descriptor_sets[2];
      write_zpd_fsi_counter.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_zpd_fsi_counter.pNext = nullptr;
      write_zpd_fsi_counter.dstSet = set;
      write_zpd_fsi_counter.dstBinding = 2;
      write_zpd_fsi_counter.dstArrayElement = 0;
      write_zpd_fsi_counter.descriptorCount = 1;
      write_zpd_fsi_counter.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write_zpd_fsi_counter.pImageInfo = nullptr;
      write_zpd_fsi_counter.pBufferInfo =
          &zpd_fsi_counter_descriptor_buffer_info;
      write_zpd_fsi_counter.pTexelBufferView = nullptr;
    }
    dfn.vkUpdateDescriptorSets(
        device, 1 + 2 * uint32_t(edram_fragment_shader_interlock),
        write_descriptor_sets, 0, nullptr);
  };
  write_shared_memory_and_edram_set(shared_memory_and_edram_descriptor_set_,
                                    shared_memory_->buffer());
  if (create_host_shared_memory_set) {
    write_shared_memory_and_edram_set(
        shared_memory_host_and_edram_descriptor_set_,
        shared_memory_->host_buffer());
  }
  if (edram_fragment_shader_interlock) {
    zpd_fsi_counter_descriptor_buffer_ = zpd_fsi_counter_sink_buffer_;
    zpd_fsi_counter_descriptor_range_ = zpd_fsi_counter_sink_range;
  }

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize =
      kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size =
      kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage =
      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode =
      VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info,
                         nullptr, &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(
        device, gamma_ramp_buffer_,
        &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local &
         vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex)) ||
        xe::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info
                  .memoryTypeIndex))) {
      VkMemoryAllocateInfo*
          gamma_ramp_host_visible_buffer_memory_allocate_info_last =
              &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo
          gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext =
            nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image =
            VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer =
            gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(
              device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
              nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_,
                                   gamma_ramp_buffer_memory_,
                                   0) == VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0,
                              VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info
                    .memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      XELOGE("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      XELOGE("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0,
                        VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      XELOGE("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType =
      VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format =
      VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset =
        kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info,
                               nullptr, &gamma_ramp_buffer_views_[i * 2 + 1]) !=
        VK_SUCCESS) {
      XELOGE("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings =
      &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_uniform_texel_buffer_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    XELOGE(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }
  // FXAA source descriptor set layout (combined image sampler for linear
  // filtering).
  swap_descriptor_set_layout_binding.descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &swap_descriptor_set_layout_create_info, nullptr,
          &fxaa_source_descriptor_set_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA source descriptor set layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes =
      swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type =
        VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount =
        gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  // Destination storage images for compute shader output.
  // Also includes storage image for writing to FXAA source (gamma+luma output).
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount =
        kMaxFramesInFlight * 2;  // dest + FXAA source storage
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  // FXAA source combined image samplers.
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info
                                       .poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount =
        kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info,
                                 nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    XELOGE("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_dest_[i]) !=
        VK_SUCCESS) {
      XELOGE(
          "Failed to allocate the presentation destination image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &fxaa_source_descriptor_set_layout_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source image descriptor sets");
      return false;
    }
  }
  // Allocate storage image descriptor sets for writing to FXAA source.
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &fxaa_source_storage_descriptors_[i]) !=
        VK_SUCCESS) {
      XELOGE("Failed to allocate the FXAA source storage descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType =
      VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView =
        &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0,
                               nullptr);
  }

  // Gamma ramp application compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetDest] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_push_constant_range;
  swap_apply_gamma_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_push_constant_range.offset = 0;
  swap_apply_gamma_push_constant_range.size = sizeof(ApplyGammaConstants);
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
          &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipelines.
  VkShaderModule swap_apply_gamma_table_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_cs,
          sizeof(shaders::apply_gamma_table_cs));
  VkShaderModule swap_apply_gamma_pwl_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::apply_gamma_pwl_cs,
                                           sizeof(shaders::apply_gamma_pwl_cs));
  VkShaderModule swap_apply_gamma_table_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_table_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  VkShaderModule swap_apply_gamma_pwl_fxaa_luma_shader_module =
      ui::vulkan::util::CreateShaderModule(
          vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_cs,
          sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_table_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_table_fxaa_luma_shader_module == VK_NULL_HANDLE ||
      swap_apply_gamma_pwl_fxaa_luma_shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to create the gamma ramp application compute shader modules");
    if (swap_apply_gamma_table_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_pwl_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                                nullptr);
    }
    if (swap_apply_gamma_table_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
    }
    if (swap_apply_gamma_pwl_fxaa_luma_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(
          device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.stage.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.stage.flags = 0;
  swap_apply_gamma_pipeline_create_info.stage.stage =
      VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_pipeline_create_info.stage.pName = "main";
  swap_apply_gamma_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_create_info.layout =
      swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;

  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_create_result =
      dfn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                   &swap_apply_gamma_pipeline_create_info,
                                   nullptr, &swap_apply_gamma_pwl_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_table_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  swap_apply_gamma_pipeline_create_info.stage.module =
      swap_apply_gamma_pwl_fxaa_luma_shader_module;
  VkResult swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result =
      dfn.vkCreateComputePipelines(
          device, VK_NULL_HANDLE, 1, &swap_apply_gamma_pipeline_create_info,
          nullptr, &swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_table_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pwl_shader_module,
                            nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_table_fxaa_luma_shader_module, nullptr);
  dfn.vkDestroyShaderModule(
      device, swap_apply_gamma_pwl_fxaa_luma_shader_module, nullptr);
  if (swap_apply_gamma_pipeline_256_entry_table_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_create_result != VK_SUCCESS ||
      swap_apply_gamma_pipeline_256_entry_table_fxaa_luma_create_result !=
          VK_SUCCESS ||
      swap_apply_gamma_pipeline_pwl_fxaa_luma_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the gamma ramp application compute pipelines");
    return false;
  }

  // FXAA sampler (linear filtering, clamp to edge).
  VkSamplerCreateInfo fxaa_sampler_create_info;
  fxaa_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  fxaa_sampler_create_info.pNext = nullptr;
  fxaa_sampler_create_info.flags = 0;
  fxaa_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  fxaa_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  fxaa_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  fxaa_sampler_create_info.mipLodBias = 0.0f;
  fxaa_sampler_create_info.anisotropyEnable = VK_FALSE;
  fxaa_sampler_create_info.maxAnisotropy = 1.0f;
  fxaa_sampler_create_info.compareEnable = VK_FALSE;
  fxaa_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  fxaa_sampler_create_info.minLod = 0.0f;
  fxaa_sampler_create_info.maxLod = 0.0f;
  fxaa_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  fxaa_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &fxaa_sampler_create_info, nullptr,
                          &fxaa_sampler_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA sampler");
    return false;
  }

  // FXAA pipeline layout.
  // set=0: destination storage image (rgb10_a2)
  // set=1: source combined image sampler (FXAA source with luma in alpha)
  std::array<VkDescriptorSetLayout, 2> fxaa_descriptor_set_layouts = {
      swap_descriptor_set_layout_storage_image_,
      fxaa_source_descriptor_set_layout_,
  };
  VkPushConstantRange fxaa_push_constant_range;
  fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_push_constant_range.offset = 0;
  fxaa_push_constant_range.size = sizeof(FxaaConstants);
  VkPipelineLayoutCreateInfo fxaa_pipeline_layout_create_info;
  fxaa_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  fxaa_pipeline_layout_create_info.pNext = nullptr;
  fxaa_pipeline_layout_create_info.flags = 0;
  fxaa_pipeline_layout_create_info.setLayoutCount =
      uint32_t(fxaa_descriptor_set_layouts.size());
  fxaa_pipeline_layout_create_info.pSetLayouts =
      fxaa_descriptor_set_layouts.data();
  fxaa_pipeline_layout_create_info.pushConstantRangeCount = 1;
  fxaa_pipeline_layout_create_info.pPushConstantRanges =
      &fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &fxaa_pipeline_layout_create_info,
                                 nullptr,
                                 &fxaa_pipeline_layout_) != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA pipeline layout");
    return false;
  }

  // FXAA compute pipelines.
  VkShaderModule fxaa_shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs));
  VkShaderModule fxaa_extreme_shader_module =
      ui::vulkan::util::CreateShaderModule(vulkan_device,
                                           shaders::fxaa_extreme_cs,
                                           sizeof(shaders::fxaa_extreme_cs));
  if (fxaa_shader_module == VK_NULL_HANDLE ||
      fxaa_extreme_shader_module == VK_NULL_HANDLE) {
    XELOGE("Failed to create the FXAA compute shader modules");
    if (fxaa_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
    }
    if (fxaa_extreme_shader_module != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
    }
    return false;
  }

  VkComputePipelineCreateInfo fxaa_pipeline_create_info;
  fxaa_pipeline_create_info.sType =
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  fxaa_pipeline_create_info.pNext = nullptr;
  fxaa_pipeline_create_info.flags = 0;
  fxaa_pipeline_create_info.stage.sType =
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fxaa_pipeline_create_info.stage.pNext = nullptr;
  fxaa_pipeline_create_info.stage.flags = 0;
  fxaa_pipeline_create_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  fxaa_pipeline_create_info.stage.pName = "main";
  fxaa_pipeline_create_info.stage.pSpecializationInfo = nullptr;
  fxaa_pipeline_create_info.layout = fxaa_pipeline_layout_;
  fxaa_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  fxaa_pipeline_create_info.basePipelineIndex = -1;

  fxaa_pipeline_create_info.stage.module = fxaa_shader_module;
  VkResult fxaa_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_pipeline_);
  fxaa_pipeline_create_info.stage.module = fxaa_extreme_shader_module;
  VkResult fxaa_extreme_pipeline_create_result = dfn.vkCreateComputePipelines(
      device, VK_NULL_HANDLE, 1, &fxaa_pipeline_create_info, nullptr,
      &fxaa_extreme_pipeline_);
  dfn.vkDestroyShaderModule(device, fxaa_shader_module, nullptr);
  dfn.vkDestroyShaderModule(device, fxaa_extreme_shader_module, nullptr);
  if (fxaa_pipeline_create_result != VK_SUCCESS ||
      fxaa_extreme_pipeline_create_result != VK_SUCCESS) {
    XELOGE("Failed to create the FXAA compute pipelines");
    return false;
  }

  // Resolve downscale compute shader for scaled resolution readback.
  // Descriptor set layout: binding 0 = source storage buffer,
  //                        binding 1 = destination storage buffer.
  {
    std::array<VkDescriptorSetLayoutBinding, 2> resolve_downscale_bindings;
    // Source buffer (readonly).
    resolve_downscale_bindings[0].binding = 0;
    resolve_downscale_bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[0].descriptorCount = 1;
    resolve_downscale_bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[0].pImmutableSamplers = nullptr;
    // Destination buffer (writeonly).
    resolve_downscale_bindings[1].binding = 1;
    resolve_downscale_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_bindings[1].descriptorCount = 1;
    resolve_downscale_bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_bindings[1].pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo resolve_downscale_set_layout_create_info;
    resolve_downscale_set_layout_create_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    resolve_downscale_set_layout_create_info.pNext = nullptr;
    resolve_downscale_set_layout_create_info.flags = 0;
    resolve_downscale_set_layout_create_info.bindingCount =
        uint32_t(resolve_downscale_bindings.size());
    resolve_downscale_set_layout_create_info.pBindings =
        resolve_downscale_bindings.data();
    if (dfn.vkCreateDescriptorSetLayout(
            device, &resolve_downscale_set_layout_create_info, nullptr,
            &resolve_downscale_descriptor_set_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale descriptor set layout");
      return false;
    }

    // Pipeline layout with push constants.
    VkPushConstantRange resolve_downscale_push_constant_range;
    resolve_downscale_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_push_constant_range.offset = 0;
    resolve_downscale_push_constant_range.size =
        sizeof(ResolveDownscaleConstants);

    VkPipelineLayoutCreateInfo resolve_downscale_pipeline_layout_create_info;
    resolve_downscale_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_downscale_pipeline_layout_create_info.pNext = nullptr;
    resolve_downscale_pipeline_layout_create_info.flags = 0;
    resolve_downscale_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_downscale_pipeline_layout_create_info.pSetLayouts =
        &resolve_downscale_descriptor_set_layout_;
    resolve_downscale_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_downscale_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_downscale_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_downscale_pipeline_layout_create_info, nullptr,
            &resolve_downscale_pipeline_layout_) != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale pipeline layout");
      return false;
    }

    // Compute pipeline.
    VkShaderModule resolve_downscale_shader_module =
        ui::vulkan::util::CreateShaderModule(
            vulkan_device, shaders::resolve_downscale_cs,
            sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_shader_module == VK_NULL_HANDLE) {
      XELOGE("Failed to create the resolve downscale shader module");
      return false;
    }

    VkComputePipelineCreateInfo resolve_downscale_pipeline_create_info;
    resolve_downscale_pipeline_create_info.sType =
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.pNext = nullptr;
    resolve_downscale_pipeline_create_info.flags = 0;
    resolve_downscale_pipeline_create_info.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    resolve_downscale_pipeline_create_info.stage.pNext = nullptr;
    resolve_downscale_pipeline_create_info.stage.flags = 0;
    resolve_downscale_pipeline_create_info.stage.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_downscale_pipeline_create_info.stage.module =
        resolve_downscale_shader_module;
    resolve_downscale_pipeline_create_info.stage.pName = "main";
    resolve_downscale_pipeline_create_info.stage.pSpecializationInfo = nullptr;
    resolve_downscale_pipeline_create_info.layout =
        resolve_downscale_pipeline_layout_;
    resolve_downscale_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
    resolve_downscale_pipeline_create_info.basePipelineIndex = -1;

    VkResult resolve_downscale_pipeline_result = dfn.vkCreateComputePipelines(
        device, VK_NULL_HANDLE, 1, &resolve_downscale_pipeline_create_info,
        nullptr, &resolve_downscale_pipeline_);
    dfn.vkDestroyShaderModule(device, resolve_downscale_shader_module, nullptr);
    if (resolve_downscale_pipeline_result != VK_SUCCESS) {
      XELOGE("Failed to create the resolve downscale compute pipeline");
      return false;
    }

    // Descriptor pool chain for resolve downscale shader.
    // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
    // Each pool has 64 sets with 2 storage buffers each.
    VkDescriptorPoolSize resolve_downscale_pool_size;
    resolve_downscale_pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    resolve_downscale_pool_size.descriptorCount = 128;  // 64 sets * 2 buffers
    resolve_downscale_descriptor_pool_chain_ =
        std::make_unique<ui::vulkan::VulkanDescriptorPoolChain>(
            vulkan_device, 0, 64, &resolve_downscale_pool_size, 1,
            resolve_downscale_descriptor_set_layout_);
  }

  // Initialize the ZPD occlusion query pool and resources.
  zpd_host_query_pool_ = std::make_unique<VulkanZPDQueryPool>();
  EnsureZPDQueryResources();

  cvars::log_gpu_frame_time_breakdown = true;
    
  // Per-submission GPU timestamps for the frame time breakdown diagnostics.
  if (cvars::log_gpu_frame_time_breakdown &&
      vulkan_device->properties().timestampPeriod > 0.0f) {
    VkQueryPoolCreateInfo timestamp_pool_create_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    timestamp_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    timestamp_pool_create_info.queryCount = kFrameTimestampSlots * 2;
    if (dfn.vkCreateQueryPool(device, &timestamp_pool_create_info, nullptr,
                              &frame_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, kFrameTimestampSlots * 2 * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              frame_timestamp_buffer_, frame_timestamp_buffer_memory_, nullptr,
              &frame_timestamp_buffer_size_) ||
          dfn.vkMapMemory(device, frame_timestamp_buffer_memory_, 0,
                          VK_WHOLE_SIZE, 0,
                          reinterpret_cast<void**>(
                              &frame_timestamp_mapping_)) != VK_SUCCESS) {
        XELOGW(
            "VulkanCommandProcessor: No readback buffer for frame timestamps; "
            "GPU execution time reporting disabled");
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               frame_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               frame_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               frame_timestamp_pool_);
        frame_timestamp_mapping_ = nullptr;
      }
    }
  }
  // Per-resolve GPU timestamps, ring-buffered per submission; only meaningful
  // alongside the per-submission pair above.
  if (frame_timestamp_mapping_ && cvars::log_gpu_frame_time_breakdown_passes) {
    VkQueryPoolCreateInfo resolve_ts_pool_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    resolve_ts_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    resolve_ts_pool_info.queryCount = kResolveTimestampPairsPerSubmission *
                                      kResolveTimestampRingSubmissions *
                                      kResolveTimestampsPerResolve;
    if (dfn.vkCreateQueryPool(device, &resolve_ts_pool_info, nullptr,
                              &resolve_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, resolve_ts_pool_info.queryCount * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              resolve_timestamp_buffer_, resolve_timestamp_buffer_memory_,
              nullptr, &resolve_timestamp_buffer_size_) ||
          dfn.vkMapMemory(device, resolve_timestamp_buffer_memory_, 0,
                          VK_WHOLE_SIZE, 0,
                          reinterpret_cast<void**>(
                              &resolve_timestamp_mapping_)) != VK_SUCCESS) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               resolve_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(
            dfn.vkFreeMemory, device, resolve_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               resolve_timestamp_pool_);
        resolve_timestamp_mapping_ = nullptr;
      }
    }
  }
  // Per-render-pass timestamps, bucketed by framebuffer extent.
  if (frame_timestamp_mapping_ && cvars::log_gpu_frame_time_breakdown_passes) {
    VkQueryPoolCreateInfo pass_ts_pool_info = {
        VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    pass_ts_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    pass_ts_pool_info.queryCount =
        kPassTimestampPairsPerSubmission * kPassTimestampRingSubmissions * 2;
    if (dfn.vkCreateQueryPool(device, &pass_ts_pool_info, nullptr,
                              &pass_timestamp_pool_) == VK_SUCCESS) {
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, pass_ts_pool_info.queryCount * sizeof(uint64_t),
              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              ui::vulkan::util::MemoryPurpose::kReadback,
              pass_timestamp_buffer_, pass_timestamp_buffer_memory_, nullptr,
              &pass_timestamp_buffer_size_) ||
          dfn.vkMapMemory(
              device, pass_timestamp_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
              reinterpret_cast<void**>(&pass_timestamp_mapping_)) !=
              VK_SUCCESS) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                               pass_timestamp_buffer_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                               pass_timestamp_buffer_memory_);
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                               pass_timestamp_pool_);
        pass_timestamp_mapping_ = nullptr;
      }
    }
  }

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));
  // ZPD FSI counter uses UINT32_MAX as its skip sentinel outside query draws.
  system_constants_.zpd_fsi_counter_index = UINT32_MAX;
  zpd_fsi_counter_index_force_update_ = true;

  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();

  ResetResolveReadWatch();

  ShutdownZPDQueryResources();
  zpd_host_query_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (frame_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, frame_timestamp_buffer_memory_);
    frame_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         frame_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         frame_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         frame_timestamp_pool_);
  vk_submit_times_.clear();
  frame_timestamp_prev_end_ = 0;
  if (resolve_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, resolve_timestamp_buffer_memory_);
    resolve_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         resolve_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         resolve_timestamp_pool_);
  resolve_ts_count_ = 0;
  if (pass_timestamp_mapping_) {
    dfn.vkUnmapMemory(device, pass_timestamp_buffer_memory_);
    pass_timestamp_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         pass_timestamp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         pass_timestamp_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device,
                                         pass_timestamp_pool_);
  pass_ts_count_ = 0;
  pass_ts_open_pair_ = UINT32_MAX;
  pass_bucket_stats_.clear();

  DestroyScratchBuffer();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);

  // FXAA cleanup.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         fxaa_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device,
                                         fxaa_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device,
                                         fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         fxaa_source_memory_);
  fxaa_source_width_ = 0;
  fxaa_source_height_ = 0;
  fxaa_source_last_submission_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         fxaa_source_descriptor_set_layout_);

  // Memexport readback snapshot cleanup.
  if (memexport_snapshot_mapping_) {
    dfn.vkUnmapMemory(device, memexport_snapshot_memory_);
    memexport_snapshot_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         memexport_snapshot_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         memexport_snapshot_memory_);
  memexport_snapshot_size_ = 0;
  // Deferred readbacks are left only by a lost device.
  pending_memexport_readbacks_.clear();
  pending_memexport_snapshot_used_ = 0;
  memexport_readbacks_in_submission_ = false;

  // Resolve downscale cleanup.
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      resolve_downscale_descriptor_set_layout_);
  resolve_downscale_descriptor_pool_chain_.reset();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                           gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorPool, device,
      shared_memory_and_edram_descriptor_pool_);
  // Both sets are freed with the pool - drop the routing handle so it stays
  // null (routing disabled) until the pool is recreated.
  shared_memory_host_and_edram_descriptor_set_ = VK_NULL_HANDLE;
  ResetMemexportPages();
  zpd_fsi_counter_descriptor_buffer_ = VK_NULL_HANDLE;
  zpd_fsi_counter_descriptor_range_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         zpd_fsi_counter_sink_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         zpd_fsi_counter_sink_buffer_memory_);

  texture_cache_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(
        device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair :
       descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second,
                                     nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(
        dfn.vkDestroyDescriptorSetLayout, device,
        descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device, descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  deferred_setup_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // Fetch (0x4800-0x48BF) and bool/loop (0x4900-0x4927) constants: the
  // derived state (texture bindings, fetch/bool-loop UBO contents) is a pure
  // function of the register value, and games commonly re-emit identical
  // constants every draw — a same-value write doesn't need to dirty
  // anything. The previous value must be read before the base class stores
  // the new one. The two ranges are checked as one span; the gap registers
  // in between don't consume the flag.
  bool constant_value_unchanged = false;
  if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 &&
      cvars::vulkan_skip_redundant_fetch_constant_writes) {
    constant_value_unchanged = register_file_->values[index] == value;
  }

  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index =
          (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    if (!constant_value_unchanged) {
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    if (!constant_value_unchanged) {
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
      uint32_t fetch_slot =
          (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
      uint32_t fetch_slot_bit_clear = ~(uint32_t(1) << fetch_slot);
      current_samplers_fetch_up_to_date_vertex_ &= fetch_slot_bit_clear;
      current_samplers_fetch_up_to_date_pixel_ &= fetch_slot_bit_clear;
      fetch_constants_decode_needed_ |= ~fetch_slot_bit_clear;
      if (texture_cache_) {
        texture_cache_->TextureFetchConstantWritten(fetch_slot);
      }
    }
  }
}
void VulkanCommandProcessor::WriteShaderConstantsFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  if (frame_open_) {
    // One usage-map scan for the whole range instead of a map test per
    // register (the float constant maps are indexed by vec4 constant).
    constexpr uint32_t kFloatVertexBit =
        UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex;
    constexpr uint32_t kFloatPixelBit =
        UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel;
    uint32_t map_index = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    uint32_t end_map_index =
        (start_index + num_registers + 3 - XE_GPU_REG_SHADER_CONSTANT_000_X) >>
        2;
    if (current_constant_buffers_up_to_date_ & kFloatVertexBit) {
      uint32_t vertex_end = std::min(end_map_index, UINT32_C(256));
      for (uint32_t i = map_index; i < vertex_end; ++i) {
        if (current_float_constant_map_vertex_[i >> 6] &
            (UINT64_C(1) << (i & 63))) {
          current_constant_buffers_up_to_date_ &= ~kFloatVertexBit;
          break;
        }
      }
    }
    if (end_map_index > 256 &&
        (current_constant_buffers_up_to_date_ & kFloatPixelBit)) {
      for (uint32_t i = std::max(map_index, UINT32_C(256)); i < end_map_index;
           ++i) {
        uint32_t pixel_index = i - 256;
        if (current_float_constant_map_pixel_[pixel_index >> 6] &
            (UINT64_C(1) << (pixel_index & 63))) {
          current_constant_buffers_up_to_date_ &= ~kFloatPixelBit;
          break;
        }
      }
    }
  }
  xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                 num_registers);
}

void VulkanCommandProcessor::WriteBoolLoopFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  bool changed;
  if (cvars::vulkan_skip_redundant_fetch_constant_writes) {
    // Same value-compare semantics as the per-register path.
    changed = false;
    for (uint32_t i = 0; i < num_registers; ++i) {
      uint32_t value = xe::load_and_swap<uint32_t>(base + i);
      changed |= register_file_->values[start_index + i] != value;
      register_file_->values[start_index + i] = value;
    }
  } else {
    changed = true;
    xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                   num_registers);
  }
  if (changed) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  }
}

void VulkanCommandProcessor::WriteFetchFromMem(uint32_t start_index,
                                               uint32_t* base,
                                               uint32_t num_registers) {
  if (!cvars::vulkan_skip_redundant_fetch_constant_writes) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    uint32_t first_slot =
        (start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
    uint32_t last_slot = (start_index + num_registers - 1 -
                          XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                         6;
    for (uint32_t slot = first_slot; slot <= last_slot; ++slot) {
      uint32_t slot_bit_clear = ~(UINT32_C(1) << slot);
      current_samplers_fetch_up_to_date_vertex_ &= slot_bit_clear;
      current_samplers_fetch_up_to_date_pixel_ &= slot_bit_clear;
      fetch_constants_decode_needed_ |= ~slot_bit_clear;
    }
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_slot, last_slot);
    }
    xe::copy_and_swap_32_unaligned(&register_file_->values[start_index], base,
                                   num_registers);
    return;
  }
  // Per-fetch-slot value compare, same semantics as the per-register path:
  // the identical fetch constants games re-emit every draw must not dirty
  // texture bindings, the fetch constant buffer or the sampler caches.
  bool any_changed = false;
  uint32_t index = start_index;
  uint32_t* src = base;
  uint32_t remaining = num_registers;
  while (remaining) {
    uint32_t slot = (index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6;
    uint32_t slot_end_index =
        XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (slot + 1) * 6;
    uint32_t count = std::min(remaining, slot_end_index - index);
    bool slot_changed = false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t value = xe::load_and_swap<uint32_t>(src + i);
      slot_changed |= register_file_->values[index + i] != value;
      register_file_->values[index + i] = value;
    }
    if (slot_changed) {
      any_changed = true;
      uint32_t slot_bit_clear = ~(UINT32_C(1) << slot);
      current_samplers_fetch_up_to_date_vertex_ &= slot_bit_clear;
      current_samplers_fetch_up_to_date_pixel_ &= slot_bit_clear;
      fetch_constants_decode_needed_ |= ~slot_bit_clear;
      if (texture_cache_) {
        texture_cache_->TextureFetchConstantWritten(slot);
      }
    }
    index += count;
    src += count;
    remaining -= count;
  }
  if (any_changed) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
  }
}

void VulkanCommandProcessor::WritePossiblySpecialRegistersFromMem(
    uint32_t start_index, uint32_t* base, uint32_t num_registers) {
  uint32_t end = start_index + num_registers;
  for (uint32_t index = start_index; index < end; ++index, ++base) {
    uint32_t value = xe::load_and_swap<uint32_t>(base);
    register_file_->values[index] = value;
    unsigned expr =
        (index - XE_GPU_REG_SCRATCH_REG0 < 8) |
        (index == XE_GPU_REG_COHER_STATUS_HOST) |
        ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
         (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));
    if (expr != 0) {
      HandleSpecialRegisterWrite(index, value);
    } else if (index == XE_GPU_REG_VGT_MAX_VTX_INDX ||
             index == XE_GPU_REG_VGT_MIN_VTX_INDX ||
             index == XE_GPU_REG_VGT_INDX_OFFSET ||
             index == XE_GPU_REG_VGT_DMA_SIZE ||
             index == XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL ||
             index == XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) {
    // Tessellation factor range and index parameters are in the system
    // constants buffer.
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  } else if ((index >= XE_GPU_REG_PA_CL_UCP_0_X &&
              index <= XE_GPU_REG_PA_CL_UCP_5_W) ||
             index == XE_GPU_REG_PA_CL_CLIP_CNTL) {
    // User clip planes are in the system constants buffer.
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
    }
  }
}

void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                                   uint32_t* base,
                                                   uint32_t num_registers) {
  if (!cvars::vulkan_fast_register_ranges) {
    for (uint32_t i = 0; i < num_registers; ++i) {
      uint32_t data = xe::load_and_swap<uint32_t>(base + i);
      VulkanCommandProcessor::WriteRegister(start_index + i, data);
    }
    return;
  }
  // Clamp to the register file (the per-register path warns and drops
  // out-of-bounds indices; reaching here with one means a corrupt packet
  // either way - Type0 base indices are 15-bit and can exceed the file).
  if (start_index + num_registers > RegisterFile::kRegisterCount) {
    XELOGW(
        "WriteRegistersFromMem: out-of-bounds register range [{:04X}, {:04X}) "
        "clamped",
        start_index, start_index + num_registers);
    if (start_index >= RegisterFile::kRegisterCount) {
      return;
    }
    num_registers = uint32_t(RegisterFile::kRegisterCount) - start_index;
  }
  uint32_t current_index = start_index;
  uint32_t end = start_index + num_registers;
  // Split the range into segments by side-effect family; everything outside
  // the special families is a pure store done as one bulk byte-swapped copy.
  auto do_range = [&](uint32_t range_end, auto&& callback) -> bool {
    if (current_index < range_end) {
      uint32_t count = std::min(range_end, end) - current_index;
      callback(current_index, base, count);
      current_index += count;
      base += count;
    }
    return current_index >= end;
  };
  auto regular = [this](uint32_t index, uint32_t* src, uint32_t count) {
    xe::copy_and_swap_32_unaligned(&register_file_->values[index], src, count);
  };
  if (do_range(XE_GPU_REG_SCRATCH_REG0, regular)) {
    return;
  }
  if (do_range(XE_GPU_REG_DC_LUT_30_COLOR + 1,
               [this](uint32_t index, uint32_t* src, uint32_t count) {
                 WritePossiblySpecialRegistersFromMem(index, src, count);
               })) {
    return;
  }
  if (do_range(XE_GPU_REG_SHADER_CONSTANT_000_X, regular)) {
    return;
  }
  if (do_range(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
               [this](uint32_t index, uint32_t* src, uint32_t count) {
                 WriteShaderConstantsFromMem(index, src, count);
               })) {
    return;
  }
  if (do_range(XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 + 1,
               [this](uint32_t index, uint32_t* src, uint32_t count) {
                 WriteFetchFromMem(index, src, count);
               })) {
    return;
  }
  if (do_range(XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031, regular)) {
    return;
  }
  if (do_range(XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1,
               [this](uint32_t index, uint32_t* src, uint32_t count) {
                 WriteBoolLoopFromMem(index, src, count);
               })) {
    return;
  }
  do_range(RegisterFile::kRegisterCount, regular);
}

void VulkanCommandProcessor::WriteRegisterRangeFromRing(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  if (!cvars::vulkan_fast_register_ranges) {
    CommandProcessor::WriteRegisterRangeFromRing(ring, base, num_registers);
    return;
  }
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  if (XE_LIKELY(!range.second)) {
    WriteRegistersFromMem(
        base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
        num_registers);
    ring->EndRead(range);
  } else {
    WriteRegisterRangeFromRing_WraparoundCase(ring, base, num_registers);
  }
}

XE_NOINLINE
void VulkanCommandProcessor::WriteRegisterRangeFromRing_WraparoundCase(
    xe::RingBuffer* ring, uint32_t base, uint32_t num_registers) {
  RingBuffer::ReadRange range =
      ring->BeginRead(num_registers * sizeof(uint32_t));
  // Callers (ExecutePacketType0/3) guarantee the ring holds the whole range;
  // BeginRead clamps to capacity, which would make the second-half copy read
  // out of bounds if this were ever violated.
  assert_true(size_t(range.first_length) + size_t(range.second_length) ==
              num_registers * sizeof(uint32_t));
  uint32_t num_first = uint32_t(range.first_length / sizeof(uint32_t));
  WriteRegistersFromMem(
      base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
      num_first);
  WriteRegistersFromMem(
      base + num_first,
      reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
      num_registers - num_first);
  ring->EndRead(range);
}
void VulkanCommandProcessor::SparseBindBuffer(
    VkBuffer buffer, uint32_t bind_count, const VkSparseMemoryBind* binds,
    VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds,
                              binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::ReassertGpuPowerControlIfDue() {
#if defined(__ANDROID__)
  const int32_t period_seconds = cvars::adrenotools_turbo_reassert_seconds;
  if (period_seconds <= 0) {
    return;
  }
  const uint32_t resumes = resume_count();
  const uint64_t now_ms = uint64_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  const bool after_resume = resumes != gpu_power_resume_count_seen_;
  if (!after_resume &&
      now_ms - gpu_power_last_request_ms_ < uint64_t(period_seconds) * 1000) {
    return;
  }
  gpu_power_resume_count_seen_ = resumes;
  gpu_power_last_request_ms_ = now_ms;
  // Logged after a resume; the periodic requests only log failures.
  ui::vulkan::RequestGpuPowerControl(after_resume ? "after resume" : nullptr);
#endif
}

void VulkanCommandProcessor::WriteFrameDumpVertexScans() {
  if (frame_dump_vertex_scans_.empty() || !shared_memory_->IsHostMapped() ||
      !AwaitAllQueueOperationsCompletion()) {
    frame_dump_vertex_scans_.clear();
    return;
  }
  // One line per draw: the vertex index range it reads (with VGT_INDX_OFFSET)
  // and, per floating-point attribute, the vertices the fetch constant's size
  // covers, the largest finite magnitude, and how many components are
  // infinite, NaN or beyond 1e5 (with the first such vertex) - from the GPU's
  // copy, which is what was drawn.
  std::vector<uint8_t> bytes;
  auto read = [&](uint64_t base, uint64_t length) -> const uint8_t* {
    if (!length || base + length > SharedMemory::kBufferSize) {
      return nullptr;
    }
    bytes.resize(size_t(length));
    return shared_memory_->ReadHostMapped(uint32_t(base), uint32_t(length),
                                          bytes.data())
               ? bytes.data()
               : nullptr;
  };
  auto half_to_float = [](uint32_t half) {
    const uint32_t sign = (half & 0x8000) << 16;
    const uint32_t exponent = (half >> 10) & 0x1F;
    const uint32_t mantissa = half & 0x3FF;
    if (!exponent) {
      const float value = std::ldexp(float(mantissa), -24);
      return sign ? -value : value;
    }
    const uint32_t bits =
        sign | (exponent == 0x1F ? 0x7F800000 : (exponent + 112) << 23) |
        (mantissa << 13);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  };
  constexpr uint32_t kMaxVertices = 1 << 16;
  // The frame now, as the page history counts it.
  const std::string frame_line =
      fmt::format("VF {}\n", uint32_t(frame_current_) + 1);
  std::fwrite(frame_line.data(), 1, frame_line.size(), frame_dump_file_);
  frame_dump_vertex_files_ = 0;
  frame_dump_broken_draws_ = 0;
  for (const FrameDumpVertexScan& scan : frame_dump_vertex_scans_) {
    uint32_t index_min = UINT32_MAX, index_max = 0;
    if (scan.index_base) {
      const uint32_t index_size =
          scan.index_format == xenos::IndexFormat::kInt32 ? 4 : 2;
      const uint8_t* indices =
          read(scan.index_base, uint64_t(scan.index_count) * index_size);
      for (uint32_t i = 0; indices && i < scan.index_count; ++i) {
        uint32_t index;
        if (index_size == 4) {
          uint32_t value;
          std::memcpy(&value, indices + i * 4, sizeof(value));
          index = xenos::GpuSwap(value, scan.index_endian);
          if (index == UINT32_MAX) {
            continue;
          }
        } else {
          uint16_t value;
          std::memcpy(&value, indices + i * 2, sizeof(value));
          index = xenos::GpuSwap(value, scan.index_endian);
          if (index == UINT16_MAX) {
            continue;
          }
        }
        index_min = std::min(index_min, index);
        index_max = std::max(index_max, index);
      }
    } else if (scan.index_count) {
      index_min = 0;
      index_max = scan.index_count - 1;
    }
    if (index_min > index_max) {
      continue;
    }
    index_min += scan.index_offset;
    index_max += scan.index_offset;
    // And VGT_MIN_VTX_INDX..VGT_MAX_VTX_INDX, which the vertex grouper clamps
    // indices to.
    std::string line = fmt::format("V {} ix {}-{} clamp {}-{}", scan.draw,
                                   index_min, index_max, scan.index_clamp_min,
                                   scan.index_clamp_max);
    bool broken = false;
    for (const FrameDumpVertexScan::Attribute& attribute : scan.attributes) {
      const bool half =
          attribute.format == xenos::VertexFormat::k_16_16_FLOAT ||
          attribute.format == xenos::VertexFormat::k_16_16_16_16_FLOAT;
      const uint32_t words =
          half ? (attribute.format == xenos::VertexFormat::k_16_16_FLOAT ? 1
                                                                         : 2)
               : uint32_t(xenos::GetVertexFormatComponentCount(
                     attribute.format));
      const uint32_t stride = attribute.stride_words * 4;
      line += fmt::format(" a{}.{}.{}", attribute.fetch_constant,
                          attribute.offset_words, uint32_t(attribute.format));
      if (!stride || attribute.offset_words < 0 ||
          uint32_t(attribute.offset_words) + words > attribute.stride_words) {
        line += ":layout?";
        continue;
      }
      const uint32_t last =
          std::min(index_max, index_min + (kMaxVertices - 1));
      const uint8_t* data =
          read(attribute.base + uint64_t(index_min) * stride,
               uint64_t(last - index_min + 1) * stride);
      if (!data) {
        line += ":unreadable";
        continue;
      }
      float max_abs = 0.0f;
      uint32_t infinite = 0, nan = 0, huge = 0, first_bad = UINT32_MAX;
      for (uint32_t vertex = index_min; vertex <= last; ++vertex) {
        const uint8_t* vertex_data = data + (vertex - index_min) * stride +
                                     attribute.offset_words * 4;
        bool bad = false;
        for (uint32_t word_index = 0; word_index < words; ++word_index) {
          uint32_t word;
          std::memcpy(&word, vertex_data + word_index * 4, sizeof(word));
          word = xenos::GpuSwap(word, attribute.endian);
          float values[2];
          uint32_t value_count = 1;
          if (half) {
            values[0] = half_to_float(word & 0xFFFF);
            values[1] = half_to_float(word >> 16);
            value_count = 2;
          } else {
            std::memcpy(&values[0], &word, sizeof(float));
          }
          for (uint32_t i = 0; i < value_count; ++i) {
            if (std::isnan(values[i])) {
              ++nan;
              bad = true;
            } else if (std::isinf(values[i])) {
              ++infinite;
              bad = true;
            } else {
              const float magnitude = std::fabs(values[i]);
              max_abs = std::max(max_abs, magnitude);
              if (magnitude > 1e5f) {
                ++huge;
                bad = true;
              }
            }
          }
        }
        if (bad && first_bad == UINT32_MAX) {
          first_bad = vertex;
        }
      }
      line += fmt::format(":n{}:max{:g}", attribute.size / stride, max_abs);
      if (infinite || nan || huge) {
        broken = true;
        line += fmt::format(":inf{}:nan{}:huge{}:at{}", infinite, nan, huge,
                            first_bad);
        // The page of the first such vertex: its state now and its history
        // (frames + 1, 0 for never), and the vertices from there, raw.
        const uint32_t bad_address = attribute.base + first_bad * stride +
                                     uint32_t(attribute.offset_words) * 4;
        const SharedMemory::PageHistory history =
            shared_memory_->GetPageHistory(bad_address);
        line += fmt::format(
            ":page{:08X}:valid{}:gpu{}:up{}:inv{}:gw{}:copy{}/{}",
            bad_address, uint32_t(shared_memory_->IsRangeValid(bad_address, 1)),
            uint32_t(shared_memory_->IsRangeGpuWritten(bad_address, 1)),
            history.upload, history.invalidation, history.gpu_write,
            history.guest_copy, uint32_t(history.guest_copy_source));
        if (frame_dump_vertex_files_ < 16) {
          ++frame_dump_vertex_files_;
          const uint32_t raw_first = first_bad - index_min;
          const uint32_t raw_count = std::min(64u, last - first_bad + 1);
          FILE* file = xe::filesystem::OpenFile(
              frame_dump_dir_ / fmt::format("v{}_{}_{}_{:08X}.bin", scan.draw,
                                            attribute.fetch_constant,
                                            attribute.offset_words,
                                            bad_address),
              "wb");
          if (file) {
            std::fwrite(data + size_t(raw_first) * stride, 1,
                        size_t(raw_count) * stride, file);
            std::fclose(file);
          }
        }
      }
      if (last != index_max) {
        line += ":truncated";
      }
    }
    line += '\n';
    std::fwrite(line.data(), 1, line.size(), frame_dump_file_);
    if (!broken || frame_dump_broken_draws_ >= 4) {
      continue;
    }
    ++frame_dump_broken_draws_;
    // A broken draw in full: its index buffer and the vertex range of every
    // binding (ib<draw>.bin, vb<draw>_<fetch constant>.bin, from the GPU's
    // copy), and the history of each of their pages ("H <draw> <page> upload
    // invalidation gpu_write@start+length guest_copy/source valid gpu").
    auto write_file = [&](const std::string& name, const uint8_t* file_data,
                          size_t file_size) {
      FILE* file = xe::filesystem::OpenFile(frame_dump_dir_ / name, "wb");
      if (file) {
        std::fwrite(file_data, 1, file_size, file);
        std::fclose(file);
      }
    };
    std::string history_lines;
    auto append_history = [&](const char* what, uint64_t start,
                              uint64_t length) {
      if (!length || start >= SharedMemory::kBufferSize) {
        return;
      }
      const uint64_t end =
          std::min(start + length, uint64_t(SharedMemory::kBufferSize));
      for (uint64_t page = start & ~uint64_t(4095); page < end;
           page += 4096) {
        const SharedMemory::PageHistory history =
            shared_memory_->GetPageHistory(uint32_t(page));
        history_lines += fmt::format(
            "H {} {} {:08X} up{} inv{} gw{}@{:08X}+{} copy{}/{} valid{} "
            "gpu{}\n",
            scan.draw, what, uint32_t(page), history.upload,
            history.invalidation, history.gpu_write, history.gpu_write_start,
            history.gpu_write_length, history.guest_copy,
            uint32_t(history.guest_copy_source),
            uint32_t(shared_memory_->IsRangeValid(uint32_t(page), 1)),
            uint32_t(shared_memory_->IsRangeGpuWritten(uint32_t(page), 1)));
      }
    };
    if (scan.index_base) {
      const uint64_t index_bytes =
          uint64_t(scan.index_count) *
          (scan.index_format == xenos::IndexFormat::kInt32 ? 4 : 2);
      if (const uint8_t* indices = read(scan.index_base, index_bytes)) {
        write_file(fmt::format("ib{}.bin", scan.draw), indices,
                   size_t(index_bytes));
      }
      append_history("ib", scan.index_base, index_bytes);
    }
    const uint32_t last = std::min(index_max, index_min + (kMaxVertices - 1));
    for (const FrameDumpVertexScan::Binding& binding : scan.bindings) {
      const uint64_t stride = uint64_t(binding.stride_words) * 4;
      if (!stride) {
        continue;
      }
      const uint64_t range_start = binding.base + index_min * stride;
      const uint64_t range_length = uint64_t(last - index_min + 1) * stride;
      if (const uint8_t* vertices = read(range_start, range_length)) {
        write_file(fmt::format("vb{}_{}.bin", scan.draw,
                               binding.fetch_constant),
                   vertices, size_t(range_length));
      }
      history_lines += fmt::format(
          "VB {} fc{} base {:08X} size {} stride {} first {} count {}\n",
          scan.draw, binding.fetch_constant, binding.base, binding.size,
          stride, index_min, last - index_min + 1);
      append_history("vb", range_start, range_length);
    }
    std::fwrite(history_lines.data(), 1, history_lines.size(),
                frame_dump_file_);
  }
  frame_dump_vertex_scans_.clear();
  BeginSubmission(true);
}

void VulkanCommandProcessor::PollFrameDump() {
  frame_draw_index_ = 0;
  shared_memory_->SetHistoryFrame(uint32_t(frame_current_));
  if (resolve_log_file_ &&
      frame_current_ >= uint64_t(std::max(cvars::resolve_log_frames, 0))) {
    std::fclose(resolve_log_file_);
    resolve_log_file_ = nullptr;
  }
  // The frame being dumped ends with this swap.
  if (frame_dump_file_) {
    // Whether the GPU's copy of each texture's base level still matches the
    // guest memory (a stale copy draws another texture's texels).
    if (!frame_dump_textures_.empty() && shared_memory_->IsHostMapped() &&
        AwaitAllQueueOperationsCompletion()) {
      std::vector<uint8_t> gpu_bytes;
      uint32_t differing_written = 0;
      for (const auto& range : frame_dump_textures_) {
        const uint32_t base = range.first, length = range.second.length;
        gpu_bytes.resize(length);
        if (!shared_memory_->ReadHostMapped(base, length, gpu_bytes.data())) {
          continue;
        }
        const uint8_t* guest_bytes = memory_->TranslatePhysical(base);
        uint32_t differing = 0, first = UINT32_MAX;
        for (uint32_t i = 0; i < length; ++i) {
          if (gpu_bytes[i] != guest_bytes[i]) {
            ++differing;
            first = std::min(first, i);
          }
        }
        std::string line = fmt::format(
            "T {:08X} {} valid {} gpu_written {} differing {} fetch "
            "{:08X}:{:08X}:{:08X}",
            base, length,
            uint32_t(shared_memory_->IsRangeValid(base, length)),
            uint32_t(shared_memory_->IsRangeGpuWritten(base, length)),
            differing, range.second.dword_0, range.second.dword_1,
            range.second.dword_2);
        if (range.second.is_3d) {
          line += " 3d";
        }
        if (differing) {
          line += fmt::format(" first +{:X}", first);
        }
        // Both copies of the first differing ones, to tell which is right, and
        // of the 3D ones (their slices are made by resolves to slices).
        if (differing || range.second.is_3d) {
          if (differing_written < 48) {
            ++differing_written;
            for (int copy = 0; copy < 2; ++copy) {
              FILE* file = xe::filesystem::OpenFile(
                  frame_dump_dir_ / fmt::format("t{:08X}_{}.bin", base,
                                                copy ? "guest" : "gpu"),
                  "wb");
              if (file) {
                std::fwrite(copy ? guest_bytes : gpu_bytes.data(), 1, length,
                            file);
                std::fclose(file);
              }
            }
          }
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), frame_dump_file_);
      }
      BeginSubmission(true);
    }
    frame_dump_textures_.clear();
    // The same for the vertex and index buffers (no copies written).
    if (!frame_dump_buffers_.empty() && shared_memory_->IsHostMapped() &&
        AwaitAllQueueOperationsCompletion()) {
      std::vector<uint8_t> gpu_bytes;
      uint32_t buffers_written = 0;
      for (const auto& range : frame_dump_buffers_) {
        const uint32_t base = range.first, length = range.second;
        gpu_bytes.resize(length);
        if (!shared_memory_->ReadHostMapped(base, length, gpu_bytes.data())) {
          continue;
        }
        const uint8_t* guest_bytes = memory_->TranslatePhysical(base);
        uint32_t differing = 0;
        for (uint32_t i = 0; i < length; ++i) {
          differing += gpu_bytes[i] != guest_bytes[i];
        }
        std::string line = fmt::format(
            "B {:08X} {} valid {} gpu_written {} differing {}\n", base, length,
            uint32_t(shared_memory_->IsRangeValid(base, length)),
            uint32_t(shared_memory_->IsRangeGpuWritten(base, length)),
            differing);
        std::fwrite(line.data(), 1, line.size(), frame_dump_file_);
        // Both copies of the first differing buffers, to tell which is right,
        // and of small ones (a few vertices).
        if ((differing || length <= 256) && buffers_written < 64) {
          ++buffers_written;
          for (int copy = 0; copy < 2; ++copy) {
            FILE* file = xe::filesystem::OpenFile(
                frame_dump_dir_ /
                    fmt::format("b{:08X}_{}.bin", base, copy ? "guest" : "gpu"),
                "wb");
            if (file) {
              std::fwrite(copy ? guest_bytes : gpu_bytes.data(), 1, length,
                          file);
              std::fclose(file);
            }
          }
        }
      }
      BeginSubmission(true);
    }
    frame_dump_buffers_.clear();
    WriteFrameDumpVertexScans();
    std::fclose(frame_dump_file_);
    frame_dump_file_ = nullptr;
    XELOGI("FrameDump: {} draws and {} resolves in {}", frame_dump_draws_,
           frame_dump_resolves_, xe::path_to_utf8(frame_dump_dir_));
  }
#if defined(__ANDROID__)
  static uint32_t frames_since_poll = 0;
  if (++frames_since_poll < 15) {
    return;
  }
  frames_since_poll = 0;
  // debug.xendroid.gpu_reload: a new value of 1 recreates the textures, 2
  // uploads all guest memory to the GPU again, 3 does both.
  char reload_value[PROP_VALUE_MAX] = {};
  __system_property_get("debug.xendroid.gpu_reload", reload_value);
  if (gpu_reload_value_ != reload_value) {
    gpu_reload_value_ = reload_value;
    const int reload = std::atoi(reload_value);
    if (reload & 2) {
      InvalidateGpuMemory();
    }
    if (reload & 1) {
      ClearCaches();
    }
    if (reload & 3) {
      XELOGI("debug.xendroid.gpu_reload: {}{}", (reload & 1) ? "textures " : "",
             (reload & 2) ? "guest memory" : "");
    }
  }
  char skip_value[PROP_VALUE_MAX] = {};
  __system_property_get("debug.xendroid.skip_draws", skip_value);
  if (skip_draws_value_ != skip_value) {
    skip_draws_value_ = skip_value;
    // <first>[-<last>], each range of the form.
    auto parse_range = [](const char* text, uint32_t& first, uint32_t& last) {
      first = UINT32_MAX;
      last = 0;
      if (*text < '0' || *text > '9') {
        return text;
      }
      char* end = nullptr;
      first = uint32_t(std::strtoul(text, &end, 10));
      last = first;
      if (*end == '-') {
        last = uint32_t(std::strtoul(end + 1, &end, 10));
      }
      return const_cast<const char*>(end);
    };
    const char* rest =
        parse_range(skip_value, skip_draws_first_, skip_draws_last_);
    skip_draws_keep_first_ = UINT32_MAX;
    skip_draws_keep_last_ = 0;
    if (*rest == '!') {
      parse_range(rest + 1, skip_draws_keep_first_, skip_draws_keep_last_);
    }
    XELOGI("debug.xendroid.skip_draws: {}", skip_draws_value_);
  }
  char memexport_log_value[PROP_VALUE_MAX] = {};
  __system_property_get("debug.xendroid.memexport_log", memexport_log_value);
  if (memexport_log_value_ != memexport_log_value) {
    memexport_log_value_ = memexport_log_value;
    memexport_log_left_ = uint32_t(std::max(std::atoi(memexport_log_value), 0));
    XELOGI("debug.xendroid.memexport_log: the next {} exports and awaits",
           memexport_log_left_);
  }
  char value[PROP_VALUE_MAX] = {};
  __system_property_get("debug.xendroid.frame_dump", value);
  if (frame_dump_value_ == value) {
    return;
  }
  frame_dump_value_ = value;
  if (!value[0] || !std::strcmp(value, "0") || !kernel_state_) {
    return;
  }
  frame_dump_dir_ =
      kernel_state_->emulator()->storage_root() / "frame_dump" / value;
  std::error_code error_code;
  std::filesystem::create_directories(frame_dump_dir_, error_code);
  frame_dump_file_ =
      xe::filesystem::OpenFile(frame_dump_dir_ / "frame.txt", "w");
  if (!frame_dump_file_) {
    XELOGE("FrameDump: can't create {}",
           xe::path_to_utf8(frame_dump_dir_ / "frame.txt"));
    return;
  }
  frame_dump_draws_ = 0;
  frame_dump_resolves_ = 0;
  frame_dump_shaders_.clear();
  frame_dump_vertex_scans_.clear();
  XELOGI("FrameDump: dumping the next frame to {}",
         xe::path_to_utf8(frame_dump_dir_));
#endif
}

void VulkanCommandProcessor::DumpFrameDraw(
    const VulkanShader& vertex_shader, const VulkanShader* pixel_shader,
    xenos::PrimitiveType prim_type, uint32_t index_count,
    const IndexBufferInfo* index_buffer_info) {
  const RegisterFile& regs = *register_file_;
  // Raw registers, decoded by the reader.
  std::string line = fmt::format(
      "D {} after_resolve {} prim {} count {} vs {:016X} ps {:016X} "
      "mode {:08X} surface {:08X} color {:08X} {:08X} {:08X} {:08X} "
      "mask {:08X} depth {:08X} depthcontrol {:08X} stencilref {:08X} "
      "blend {:08X} {:08X} {:08X} {:08X} colorcontrol {:08X} "
      "sc_mode {:08X} window_offset {:08X}",
      frame_dump_draws_++, frame_dump_resolves_, uint32_t(prim_type),
      index_count, vertex_shader.ucode_data_hash(),
      pixel_shader ? pixel_shader->ucode_data_hash() : 0,
      regs.values[XE_GPU_REG_RB_MODECONTROL],
      regs.values[XE_GPU_REG_RB_SURFACE_INFO],
      regs.values[XE_GPU_REG_RB_COLOR_INFO],
      regs.values[XE_GPU_REG_RB_COLOR1_INFO],
      regs.values[XE_GPU_REG_RB_COLOR2_INFO],
      regs.values[XE_GPU_REG_RB_COLOR3_INFO],
      regs.values[XE_GPU_REG_RB_COLOR_MASK],
      regs.values[XE_GPU_REG_RB_DEPTH_INFO],
      regs.values[XE_GPU_REG_RB_DEPTHCONTROL],
      regs.values[XE_GPU_REG_RB_STENCILREFMASK],
      regs.values[XE_GPU_REG_RB_BLENDCONTROL0],
      regs.values[XE_GPU_REG_RB_BLENDCONTROL1],
      regs.values[XE_GPU_REG_RB_BLENDCONTROL2],
      regs.values[XE_GPU_REG_RB_BLENDCONTROL3],
      regs.values[XE_GPU_REG_RB_COLORCONTROL],
      regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
      regs.values[XE_GPU_REG_PA_SC_WINDOW_OFFSET]);
  // The vertex buffers (fetch constant words and the binding's stride) and the
  // index buffer.
  auto record_buffer = [&](uint32_t base, uint32_t length) {
    if (base && length && uint64_t(base) + length <= (UINT64_C(1) << 29)) {
      uint32_t& recorded = frame_dump_buffers_[base];
      recorded = std::max(recorded, length);
    }
  };
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(binding.fetch_constant);
    line += fmt::format(" vf{}:{:08X}:{:08X}:{}", binding.fetch_constant,
                        fetch.dword_0, fetch.dword_1, binding.stride_words);
    record_buffer(uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2);
    // The words of a small buffer (a few vertices) as the guest memory holds
    // them when the draw is issued.
    if (fetch.size && fetch.size <= 64) {
      const uint32_t* words = reinterpret_cast<const uint32_t*>(
          memory_->TranslatePhysical(uint32_t(fetch.address) << 2));
      line += fmt::format(" vd{}=", binding.fetch_constant);
      for (uint32_t i = 0; i < fetch.size; ++i) {
        line += fmt::format("{}{:08X}", i ? "," : "", words[i]);
      }
    }
  }
  // The buffers the pixel shader fetches vertices from.
  if (pixel_shader) {
    for (const Shader::VertexBinding& binding :
         pixel_shader->vertex_bindings()) {
      const xenos::xe_gpu_vertex_fetch_t fetch =
          regs.GetVertexFetch(binding.fetch_constant);
      line += fmt::format(" pvf{}:{:08X}:{:08X}:{}", binding.fetch_constant,
                          fetch.dword_0, fetch.dword_1, binding.stride_words);
      record_buffer(uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2);
    }
  }
  // The bool constants (VS 0...127 then PS 128...255) and the loop
  // constants the shaders branch on.
  line += " bool";
  for (uint32_t i = 0; i < 8; ++i) {
    line += fmt::format(
        "{}{:08X}", i ? "," : " ",
        regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i]);
  }
  line += " loop";
  for (uint32_t i = 0; i < 32; ++i) {
    line += fmt::format("{}{:X}", i ? "," : " ",
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + i]);
  }
  // Clipping and the viewport transform.
  line += fmt::format(
      " clip {:08X} vte {:08X} vport {:g},{:g},{:g},{:g},{:g},{:g}",
      regs.values[XE_GPU_REG_PA_CL_CLIP_CNTL],
      regs.values[XE_GPU_REG_PA_CL_VTE_CNTL],
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
      regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
  // The float constants the vertex shader reads (c0...c255).
  {
    const Shader::ConstantRegisterMap& constants =
        vertex_shader.constant_register_map();
    for (uint32_t i = 0; i < 256; ++i) {
      if (!(constants.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) {
        continue;
      }
      float components[4];
      std::memcpy(components,
                  &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4],
                  sizeof(components));
      line += fmt::format(" vc{}={:g},{:g},{:g},{:g}", i, components[0],
                          components[1], components[2], components[3]);
    }
  }
  if (index_buffer_info && index_buffer_info->guest_base) {
    line += fmt::format(" ib:{:08X}:{}:{}:{}", index_buffer_info->guest_base,
                        uint32_t(index_buffer_info->format),
                        uint32_t(index_buffer_info->endianness),
                        uint32_t(index_buffer_info->length));
    record_buffer(index_buffer_info->guest_base,
                  uint32_t(index_buffer_info->length));
  }
  // The floating-point attributes and the indices, scanned when the frame ends.
  {
    FrameDumpVertexScan& scan = frame_dump_vertex_scans_.emplace_back();
    scan.draw = frame_dump_draws_ - 1;
    const bool indexed = index_buffer_info && index_buffer_info->guest_base;
    scan.index_base = indexed ? index_buffer_info->guest_base : 0;
    scan.index_count = indexed ? index_buffer_info->count : index_count;
    scan.index_format =
        indexed ? index_buffer_info->format : xenos::IndexFormat::kInt16;
    scan.index_endian =
        indexed ? index_buffer_info->endianness : xenos::Endian::kNone;
    scan.index_offset = regs.values[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
    scan.index_clamp_min = regs.values[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
    scan.index_clamp_max = regs.values[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
    for (const Shader::VertexBinding& binding :
         vertex_shader.vertex_bindings()) {
      const xenos::xe_gpu_vertex_fetch_t fetch =
          regs.GetVertexFetch(binding.fetch_constant);
      scan.bindings.push_back(FrameDumpVertexScan::Binding{
          binding.fetch_constant, uint32_t(fetch.address) << 2,
          uint32_t(fetch.size) << 2, binding.stride_words});
      for (const Shader::VertexBinding::Attribute& attribute :
           binding.attributes) {
        const xenos::VertexFormat format =
            attribute.fetch_instr.attributes.data_format;
        switch (format) {
          case xenos::VertexFormat::k_32_FLOAT:
          case xenos::VertexFormat::k_32_32_FLOAT:
          case xenos::VertexFormat::k_32_32_32_FLOAT:
          case xenos::VertexFormat::k_32_32_32_32_FLOAT:
          case xenos::VertexFormat::k_16_16_FLOAT:
          case xenos::VertexFormat::k_16_16_16_16_FLOAT:
            scan.attributes.push_back(FrameDumpVertexScan::Attribute{
                uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2,
                binding.stride_words,
                attribute.fetch_instr.attributes.offset,
                binding.fetch_constant, format, fetch.endian});
            break;
          default:
            break;
        }
      }
    }
  }
  // The six dwords of the fetch constant of every texture the shaders sample.
  auto append_textures = [&](const char* stage, const VulkanShader& shader) {
    for (const Shader::TextureBinding& binding : shader.texture_bindings()) {
      const xenos::xe_gpu_texture_fetch_t fetch =
          regs.GetTextureFetch(binding.fetch_constant);
      line += fmt::format(
          " {}{}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X}:{:08X}", stage,
          binding.fetch_constant, fetch.dword_0, fetch.dword_1, fetch.dword_2,
          fetch.dword_3, fetch.dword_4, fetch.dword_5);
      // The base level of a 2D texture, its blocks in 32x32 tiles when tiled.
      const FormatInfo* format_info = FormatInfo::Get(fetch.format);
      if (fetch.dimension == xenos::DataDimension::k2DOrStacked &&
          format_info && format_info->bits_per_pixel) {
        uint32_t width_blocks =
            xe::align(uint32_t(fetch.size_2d.width) + 1,
                      format_info->block_width) /
            format_info->block_width;
        uint32_t height_blocks =
            xe::align(uint32_t(fetch.size_2d.height) + 1,
                      format_info->block_height) /
            format_info->block_height;
        if (fetch.tiled) {
          width_blocks = xe::align(width_blocks, UINT32_C(32));
          height_blocks = xe::align(height_blocks, UINT32_C(32));
        } else {
          width_blocks = xe::align(
              width_blocks * format_info->bytes_per_block(), UINT32_C(256)) /
              format_info->bytes_per_block();
        }
        const uint32_t base = fetch.base_address << 12;
        const uint64_t length =
            uint64_t(width_blocks) * height_blocks *
            format_info->bytes_per_block();
        if (base && length && base + length <= (UINT64_C(1) << 29)) {
          FrameDumpTexture& recorded = frame_dump_textures_.try_emplace(
              base, FrameDumpTexture{0, fetch.dword_0, fetch.dword_1,
                                     fetch.dword_2, false}).first->second;
          recorded.length = std::max(recorded.length, uint32_t(length));
        }
      } else if (fetch.dimension == xenos::DataDimension::k3D &&
                 format_info && format_info->bits_per_pixel) {
        // The base level of a 3D texture, all its slices: 32x32x4 tiles when
        // tiled.
        uint32_t width_blocks =
            xe::align(uint32_t(fetch.size_3d.width) + 1,
                      format_info->block_width) /
            format_info->block_width;
        uint32_t height_blocks =
            xe::align(uint32_t(fetch.size_3d.height) + 1,
                      format_info->block_height) /
            format_info->block_height;
        uint32_t depth = uint32_t(fetch.size_3d.depth) + 1;
        if (fetch.tiled) {
          width_blocks = xe::align(width_blocks, UINT32_C(32));
          height_blocks = xe::align(height_blocks, UINT32_C(32));
          depth = xe::align(depth, UINT32_C(4));
        } else {
          width_blocks = xe::align(
              width_blocks * format_info->bytes_per_block(), UINT32_C(256)) /
              format_info->bytes_per_block();
        }
        const uint32_t base = fetch.base_address << 12;
        const uint64_t length = uint64_t(width_blocks) * height_blocks *
                                depth * format_info->bytes_per_block();
        if (base && length && base + length <= (UINT64_C(1) << 29)) {
          FrameDumpTexture& recorded = frame_dump_textures_.try_emplace(
              base, FrameDumpTexture{0, fetch.dword_0, fetch.dword_1,
                                     fetch.dword_2, true}).first->second;
          recorded.length = std::max(recorded.length, uint32_t(length));
          recorded.is_3d = true;
        }
      }
    }
  };
  append_textures("vt", vertex_shader);
  if (pixel_shader) {
    append_textures("pt", *pixel_shader);
    // The float constants the pixel shader reads (from c256 of the register
    // file, where Direct3D 9 puts them).
    const Shader::ConstantRegisterMap& constants =
        pixel_shader->constant_register_map();
    for (uint32_t i = 0; i < 256; ++i) {
      if (!(constants.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) {
        continue;
      }
      const uint32_t* value =
          &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + (256 + i) * 4];
      float components[4];
      std::memcpy(components, value, sizeof(components));
      line += fmt::format(" pc{}={:g},{:g},{:g},{:g}", i, components[0],
                          components[1], components[2], components[3]);
    }
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), frame_dump_file_);
  // The microcode of each shader once, as disassembled by the analysis.
  auto dump_shader = [&](const char* stage, const VulkanShader& shader) {
    if (!frame_dump_shaders_.insert(shader.ucode_data_hash()).second) {
      return;
    }
    FILE* file = xe::filesystem::OpenFile(
        frame_dump_dir_ /
            fmt::format("{}_{:016X}.txt", stage, shader.ucode_data_hash()),
        "w");
    if (file) {
      const std::string& disassembly = shader.ucode_disassembly();
      std::fwrite(disassembly.data(), 1, disassembly.size(), file);
      std::fclose(file);
    }
  };
  dump_shader("vs", vertex_shader);
  if (pixel_shader) {
    dump_shader("ps", *pixel_shader);
  }
}

bool VulkanCommandProcessor::OpenResolveLog() {
  if (!resolve_log_file_ && kernel_state_) {
    resolve_log_file_ = xe::filesystem::OpenFile(
        kernel_state_->emulator()->storage_root() / "resolve_log.txt", "w");
  }
  return resolve_log_file_ != nullptr;
}

void VulkanCommandProcessor::LogMemexportDraw(
    const VulkanShader& vertex_shader, const VulkanShader* pixel_shader,
    uint32_t index_count, const IndexBufferInfo* index_buffer_info) {
  if (!OpenResolveLog() || !shared_memory_->IsHostMapped() ||
      !AwaitAllQueueOperationsCompletion()) {
    return;
  }
  const RegisterFile& regs = *register_file_;
  // "M <frame> vs <hash> ps <hash> count <n> out <export ranges> in <what read>":
  // per range read, its pages' state (all valid, all GPU-written), the bytes
  // where the GPU's copy differs from guest memory, and the history of its
  // first page. A valid range that differs fed the export stale data.
  std::string line = fmt::format(
      "M {} vs {:016X} ps {:016X} count {} out", frame_current_,
      vertex_shader.ucode_data_hash(),
      pixel_shader ? pixel_shader->ucode_data_hash() : 0, index_count);
  // The first 32 bytes of a range in the GPU's copy, as hex.
  auto head_hex = [&](uint32_t base, uint32_t length) {
    uint8_t head[32];
    length = std::min(length, uint32_t(sizeof(head)));
    std::string hex;
    if (length && uint64_t(base) + length <= SharedMemory::kBufferSize &&
        shared_memory_->ReadHostMapped(base, length, head)) {
      for (uint32_t i = 0; i < length; ++i) {
        hex += fmt::format("{:02X}", head[i]);
      }
    }
    return hex;
  };
  for (const draw_util::MemExportRange& range : memexport_ranges_) {
    const uint32_t base = range.base_address_dwords << 2;
    line += fmt::format(" {:08X}+{}:{}", base, range.size_bytes,
                        head_hex(base, range.size_bytes));
  }
  // The float constants the vertex shader reads (c0-c255).
  line += " c";
  const Shader::ConstantRegisterMap& constants =
      vertex_shader.constant_register_map();
  for (uint32_t i = 0; i < 256; ++i) {
    if (!(constants.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) {
      continue;
    }
    const uint32_t* value =
        &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4];
    line += fmt::format(" c{}={:08X}:{:08X}:{:08X}:{:08X}", i, value[0],
                        value[1], value[2], value[3]);
  }
  if (memexport_logged_shaders_.insert(vertex_shader.ucode_data_hash())
          .second) {
    FILE* file = xe::filesystem::OpenFile(
        kernel_state_->emulator()->storage_root() /
            fmt::format("memexport_vs_{:016X}.txt",
                        vertex_shader.ucode_data_hash()),
        "w");
    if (file) {
      const std::string& disassembly = vertex_shader.ucode_disassembly();
      std::fwrite(disassembly.data(), 1, disassembly.size(), file);
      std::fclose(file);
    }
  }
  line += " in";
  std::vector<uint8_t> gpu_bytes;
  auto append_read = [&](const char* what, uint32_t base, uint32_t length) {
    length = std::min(length, uint32_t(1) << 20);
    if (!length || uint64_t(base) + length > SharedMemory::kBufferSize) {
      return;
    }
    gpu_bytes.resize(length);
    uint32_t differing = UINT32_MAX;
    if (shared_memory_->ReadHostMapped(base, length, gpu_bytes.data())) {
      const uint8_t* guest_bytes = memory_->TranslatePhysical(base);
      differing = 0;
      for (uint32_t i = 0; i < length; ++i) {
        differing += gpu_bytes[i] != guest_bytes[i];
      }
    }
    const SharedMemory::PageHistory history =
        shared_memory_->GetPageHistory(base);
    line += fmt::format(
        " {}:{:08X}+{}:valid{}:gpu{}:diff{}:up{}:inv{}:gw{}:copy{}/{}:{}",
        what, base, length,
        uint32_t(shared_memory_->IsRangeValid(base, length)),
        uint32_t(shared_memory_->IsRangeGpuWritten(base, length)), differing,
        history.upload, history.invalidation, history.gpu_write,
        history.guest_copy, uint32_t(history.guest_copy_source),
        head_hex(base, length));
  };
  for (const Shader::VertexBinding& binding : vertex_shader.vertex_bindings()) {
    const xenos::xe_gpu_vertex_fetch_t fetch =
        regs.GetVertexFetch(binding.fetch_constant);
    append_read(fmt::format("vf{}", binding.fetch_constant).c_str(),
                uint32_t(fetch.address) << 2, uint32_t(fetch.size) << 2);
  }
  if (index_buffer_info && index_buffer_info->guest_base) {
    append_read("ib", index_buffer_info->guest_base,
                uint32_t(index_buffer_info->length));
  }
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), resolve_log_file_);
  BeginSubmission(true);
}

void VulkanCommandProcessor::LogResolve(uint32_t written_address,
                                        uint32_t written_length) {
  if (!OpenResolveLog()) {
    return;
  }
  const draw_util::ResolveInfo& info = render_target_cache_->last_resolve_info();
  const bool is_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram_info =
      is_depth ? info.depth_edram_info : info.color_edram_info;
  const std::string line = fmt::format(
      "{} {} base {} pitch {} msaa {} format {} rect {},{} {}x{} dest {:08X} "
      "pitch {} height {} offset {},{} format {} endian {} number {} "
      "exp_bias {} swap {} written {:08X} {} copy_control {:08X}\n",
      frame_current_, is_depth ? "depth" : "color",
      uint32_t(edram_info.base_tiles), uint32_t(edram_info.pitch_tiles),
      1u << uint32_t(edram_info.msaa_samples), uint32_t(edram_info.format),
      uint32_t(info.coordinate_info.edram_offset_x_div_8) * 8,
      uint32_t(info.coordinate_info.edram_offset_y_div_8) * 8,
      uint32_t(info.coordinate_info.width_div_8) * 8, info.height_div_8 * 8,
      info.copy_dest_base,
      uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
      uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
      uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * 8,
      uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * 8,
      uint32_t(info.copy_dest_info.copy_dest_format),
      uint32_t(info.copy_dest_info.copy_dest_endian),
      uint32_t(info.copy_dest_info.copy_dest_number),
      int32_t(info.copy_dest_info.copy_dest_exp_bias),
      uint32_t(info.copy_dest_info.copy_dest_swap), written_address,
      written_length, info.rb_copy_control.value);
  std::fwrite(line.data(), 1, line.size(), resolve_log_file_);
}

void VulkanCommandProcessor::DumpFrameResolve(uint32_t written_address,
                                              uint32_t written_length) {
  const draw_util::ResolveInfo& info = render_target_cache_->last_resolve_info();
  const bool is_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram_info =
      is_depth ? info.depth_edram_info : info.color_edram_info;
  const uint32_t index = frame_dump_resolves_++;
  std::string line = fmt::format(
      "R {} after_draw {} copy_control {:08X} src {} base {} pitch {} msaa {} "
      "format {} 64bpp {} rect {},{} {}x{} dest {:08X} pitch {} height {} "
      "offset {},{} info {:08X} format {} endian {} number {} exp_bias {} "
      "swap {} sample {} written {:08X} {} clear_color {:08X}{:08X} "
      "clear_depth {:08X}",
      index, frame_dump_draws_, info.rb_copy_control.value,
      is_depth ? "depth" : "color", uint32_t(edram_info.base_tiles),
      uint32_t(edram_info.pitch_tiles),
      1u << uint32_t(edram_info.msaa_samples), uint32_t(edram_info.format),
      uint32_t(edram_info.format_is_64bpp),
      uint32_t(info.coordinate_info.edram_offset_x_div_8) * 8,
      uint32_t(info.coordinate_info.edram_offset_y_div_8) * 8,
      uint32_t(info.coordinate_info.width_div_8) * 8, info.height_div_8 * 8,
      info.copy_dest_base,
      uint32_t(info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
      uint32_t(info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
      uint32_t(info.copy_dest_coordinate_info.offset_x_div_8) * 8,
      uint32_t(info.copy_dest_coordinate_info.offset_y_div_8) * 8,
      info.copy_dest_info.value,
      uint32_t(info.copy_dest_info.copy_dest_format),
      uint32_t(info.copy_dest_info.copy_dest_endian),
      uint32_t(info.copy_dest_info.copy_dest_number),
      int32_t(info.copy_dest_info.copy_dest_exp_bias),
      uint32_t(info.copy_dest_info.copy_dest_swap),
      uint32_t(info.copy_dest_coordinate_info.copy_sample_select),
      written_address, written_length, info.rb_color_clear,
      info.rb_color_clear_lo, info.rb_depth_clear);
  bool read = false;
  if (written_length && shared_memory_->IsHostMapped()) {
    // Like the first uma readback of a destination: make the resolve visible
    // to the host and drain, then go on in a new submission.
    PushBufferMemoryBarrier(
        shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
        guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
    SubmitBarriers(true);
    if (AwaitAllQueueOperationsCompletion()) {
      std::vector<uint8_t> bytes(written_length);
      if (shared_memory_->ReadHostMapped(written_address, written_length,
                                         bytes.data())) {
        FILE* file = xe::filesystem::OpenFile(
            frame_dump_dir_ / fmt::format("r{:03}.bin", index), "wb");
        if (file) {
          std::fwrite(bytes.data(), 1, bytes.size(), file);
          std::fclose(file);
          read = true;
        }
      }
    }
    BeginSubmission(true);
  }
  line += read ? " data\n" : " no_data\n";
  std::fwrite(line.data(), 1, line.size(), frame_dump_file_);
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr,
                                       uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");

  // Count one presented guest frame for the debug overlay's FPS / frame-time
  // stats (the game's real frame rate, independent of the host present path).
  xe::RecordGuestPresent();

  PollDebugPropertyOverrides(*this);
  PollFrameDump();
  ReassertGpuPowerControlIfDue();

  if (!pipeline_use_.empty()) {
    // Most-used shader pairs of the traced frame, to rank against PipeStats.
    std::vector<std::pair<std::tuple<uint64_t, uint64_t, uint32_t>,
                          PipelineUse>>
        uses(pipeline_use_.begin(), pipeline_use_.end());
    std::sort(uses.begin(), uses.end(), [](const auto& a, const auto& b) {
      return a.second.draws > b.second.draws;
    });
    uint32_t total_draws = 0;
    // Pixel shader texture bindings by sign class and draws by alpha
    // handling, per pass size.
    std::map<uint32_t, std::array<uint64_t, 5>> texture_signs_by_pass;
    std::map<uint32_t, std::array<uint64_t, 4>> alpha_modes_by_pass;
    for (const auto& use : uses) {
      total_draws += use.second.draws;
      std::array<uint64_t, 5>& pass_signs =
          texture_signs_by_pass[std::get<2>(use.first)];
      std::array<uint64_t, 4>& pass_alpha_modes =
          alpha_modes_by_pass[std::get<2>(use.first)];
      pass_signs[4] += use.second.draws;
      for (uint32_t i = 0; i < 4; ++i) {
        pass_signs[i] += use.second.texture_signs[i];
        pass_alpha_modes[i] += use.second.alpha_modes[i];
      }
    }
    XELOGI("PipeUse frame {}: {} shader pairs/pass sizes, {} draws",
           bin_trace_.frame_number, uses.size(), total_draws);
    for (size_t i = 0; i < std::min(uses.size(), size_t(240)); ++i) {
      const auto& key = uses[i].first;
      const uint32_t* signs = uses[i].second.texture_signs;
      XELOGI(
          "PipeUse: VS {:016X} PS {:016X} pass {}x{} draws={} verts={} "
          "tex={}/{}/{}/{}",
          std::get<0>(key), std::get<1>(key), std::get<2>(key) >> 16,
          std::get<2>(key) & 0xFFFF, uses[i].second.draws,
          uses[i].second.vertices, signs[0], signs[1], signs[2], signs[3]);
    }
    // Whether gamma textures are common decides between converting gamma in
    // a uniform branch (spirv_texture_sign_branch) and per-draw variants.
    for (const auto& pass : texture_signs_by_pass) {
      const std::array<uint64_t, 5>& signs = pass.second;
      const uint64_t textures = signs[0] + signs[1] + signs[2] + signs[3];
      const double percent = textures ? 100.0 / double(textures) : 0.0;
      XELOGI(
          "TexSigns frame {}: pass {}x{} draws={} textures={} unsigned={:.1f}% "
          "signed={:.1f}% biased={:.1f}% gamma={:.1f}%",
          bin_trace_.frame_number, pass.first >> 16, pass.first & 0xFFFF,
          signs[4], textures, signs[0] * percent, signs[1] * percent,
          signs[2] * percent, signs[3] * percent);
    }
    // Whether draws with the alpha test but without alpha to coverage are
    // common decides on an alpha-test-only pixel shader variant.
    for (const auto& pass : alpha_modes_by_pass) {
      const std::array<uint64_t, 4>& modes = pass.second;
      const uint64_t draws = modes[0] + modes[1] + modes[2] + modes[3];
      const double percent = draws ? 100.0 / double(draws) : 0.0;
      XELOGI(
          "AlphaModes frame {}: pass {}x{} ps_draws={} no_alpha={:.1f}% "
          "test_only={:.1f}% coverage={:.1f}% other={:.1f}%",
          bin_trace_.frame_number, pass.first >> 16, pass.first & 0xFFFF,
          draws, modes[0] * percent, modes[1] * percent, modes[2] * percent,
          modes[3] * percent);
    }
    pipeline_use_.clear();
  }

  if (render_target_cache_) {
    render_target_cache_->LogResolveDetailsOnFrameEnd();
  }

  if (cvars::log_gpu_frame_time_breakdown) {
    auto& s = vk_frame_sync_stats_;
    s.frames++;
    const uint64_t now = FrameStatsNow();
    if (!s.last_report_ns) {
      s.last_report_ns = now;
    } else if (now - s.last_report_ns >= 1000000000ull) {
      const double f = static_cast<double>(s.frames);
      XELOGI(
          "VkFrameSync: {} frames | per frame: awaits={:.1f} await={:.1f}ms "
          "submissions={:.1f} resolves={:.1f} memexport_awaits={:.1f} "
          "readback_awaits={:.1f} "
          "| submit->fence avg={:.1f}ms max={:.1f}ms | blocking={:.1f} "
          "delta avg={:.2f} | gpu exec avg={:.1f}ms max={:.1f}ms "
          "gap avg={:.1f}ms | resolve_ms={:.2f} avg={:.3f} max={:.2f} "
          "dropped={} | draws={:.0f} rp_begins={:.0f} splits={:.1f} "
          "replay={:.1f}ms | resolve_clears={:.1f} in_guest_pass={:.1f} | "
          "uma readbacks={:.1f} first={:.1f} unread={:.1f} kb={:.0f}",
          s.frames, s.awaits / f, s.await_ns / f / 1e6, s.submissions / f,
          s.resolves / f, s.memexport_awaits / f, s.readback_awaits / f,
          s.sub_completions
              ? s.sub_latency_ns / static_cast<double>(s.sub_completions) / 1e6
              : 0.0,
          s.sub_latency_max_ns / 1e6, s.blocking_awaits / f,
          s.blocking_awaits
              ? s.await_delta / static_cast<double>(s.blocking_awaits)
              : 0.0,
          s.gpu_samples
              ? s.gpu_exec_ns / static_cast<double>(s.gpu_samples) / 1e6
              : 0.0,
          s.gpu_exec_max_ns / 1e6,
          s.gpu_samples
              ? s.gpu_gap_ns / static_cast<double>(s.gpu_samples) / 1e6
              : 0.0,
          s.resolve_gpu_ns / f / 1e6,
          s.resolve_gpu_samples
              ? s.resolve_gpu_ns /
                    static_cast<double>(s.resolve_gpu_samples) / 1e6
              : 0.0,
          s.resolve_gpu_max_ns / 1e6, s.resolve_ts_dropped, s.draws / f,
          s.render_pass_begins / f, s.primary_buffer_splits / f,
          s.replay_ns / f / 1e6, s.resolve_clears / f,
          s.resolve_clears_in_guest_pass / f, s.rb_uma_direct / f,
          s.rb_uma_first_use / f, s.rb_uma_unread / f,
          s.rb_uma_bytes / f / 1024.0);
      // What the replays sent to the driver per frame, and how much of the
      // state setting repeated what the command buffer already had.
      if (cvars::vulkan_replay_stats) {
        DeferredCommandBuffer::ReplayStats replay_stats;
        deferred_command_buffer_.TakeReplayStats(replay_stats);
        deferred_setup_command_buffer_.TakeReplayStats(replay_stats);
        std::string commands;
        for (size_t i = 0; i < DeferredCommandBuffer::kReplayStatCommandCount;
             ++i) {
          if (!replay_stats.commands[i]) {
            continue;
          }
          commands += fmt::format(
              " {}={:.0f}", DeferredCommandBuffer::GetReplayStatCommandName(i),
              replay_stats.commands[i] / f);
          if (replay_stats.redundant[i]) {
            commands +=
                fmt::format("(same {:.0f})", replay_stats.redundant[i] / f);
          }
        }
        XELOGI(
            "VkReplay: per frame: sets={:.0f} push_bytes={:.0f} "
            "texture_set_writes={:.0f} (same content earlier in the frame "
            "{:.0f}) texture_set_reuses={:.0f} constants_set_writes={:.0f} "
            "|{}",
            replay_stats.descriptor_sets / f,
            replay_stats.push_constant_bytes / f, s.texture_set_writes / f,
            s.texture_set_rewrites / f, s.texture_set_reuses / f,
            s.constants_set_writes / f, commands);
      }
      // The page protection changes of the memory watches: enabled by the GPU
      // side (uploads, GPU writes, resolve read watches), removed on access
      // violations, and the pages CPU writes made invalid (repeated: in the
      // same or the previous frame too, with shared_memory_page_history).
      {
        const AccessCallbackProtectStats protect =
            TakeAccessCallbackProtectStats();
        uint64_t invalidated_pages, repeated_invalidated_pages;
        shared_memory_->TakeInvalidationStats(invalidated_pages,
                                              repeated_invalidated_pages);
        XELOGI(
            "VkWatch: per frame: protects={:.1f} protect_pages={:.0f} "
            "faults={:.1f} unprotects={:.1f} unprotect_pages={:.0f} "
            "invalidated_pages={:.0f} repeated={:.0f}",
            protect.protect_calls / f, protect.protect_pages / f,
            protect.triggers / f, protect.unprotect_calls / f,
            protect.unprotect_pages / f, invalidated_pages / f,
            repeated_invalidated_pages / f);
      }
      // The textures waiting to be recreated for promotion to resolve
      // destinations (the in use and not found counts with
      // vulkan_resolve_dest_diag).
      {
        const VulkanTextureCache::PromotionQueueStats promotion =
            texture_cache_->TakePromotionQueueStats();
        XELOGI(
            "VkPromoteQueue: pending={} | per frame: recreated={:.2f} "
            "in_use={:.1f} not_found={:.1f}",
            promotion.pending, promotion.recreated / f,
            promotion.in_flight / f, promotion.not_found / f);
      }
      // Driver fence polls and waits of the submission timeline (a poll of a
      // pending fence blocks on Turnip/kgsl, see fence_collect).
      const ui::vulkan::VulkanGPUCompletionTimeline::DriverWaitStats fence =
          completion_timeline_.TakeDriverWaitStats();
      XELOGI(
          "VkFences: per frame: polls={:.1f} slow={:.1f} poll={:.2f}ms "
          "waits={:.1f} wait={:.2f}ms | bounded={}",
          fence.polls / f, fence.slow_polls / f, fence.poll_ns / f / 1e6,
          fence.waits / f, fence.wait_ns / f / 1e6,
          ui::vulkan::VulkanGPUCompletionTimeline::bounded_collection());
      // What ended the render passes (PassEndReason) - barrier kinds when
      // pending barriers did, otherwise the work that needed the pass ended.
      {
        auto ends = [&](PassEndReason reason) {
          return s.pass_ends[size_t(reason)] / f;
        };
        auto reopens = [&](PassEndReason reason) {
          return s.pass_reopens[size_t(reason)] / f;
        };
        uint64_t reopens_total = 0;
        for (uint64_t count : s.pass_reopens) {
          reopens_total += count;
        }
        XELOGI(
            "VkPassEnd: per frame: render_targets={:.1f} resolve={:.1f} "
            "textures={:.1f} shared_memory={:.1f} primitives={:.1f} "
            "query={:.1f} submission={:.1f} other={:.1f} | barriers: "
            "buffer={:.1f} image={:.1f} both={:.1f} | reopened on the same "
            "framebuffer: {:.1f} (after a resolve {:.1f}; ended by barriers: "
            "buffer={:.1f} image={:.1f} both={:.1f}; submission={:.1f})",
            ends(PassEndReason::kRenderTargets),
            ends(PassEndReason::kResolve), ends(PassEndReason::kTextures),
            ends(PassEndReason::kSharedMemory),
            ends(PassEndReason::kPrimitiveProcessor),
            ends(PassEndReason::kQuery), ends(PassEndReason::kSubmission),
            ends(PassEndReason::kOther), ends(PassEndReason::kBufferBarriers),
            ends(PassEndReason::kImageBarriers),
            ends(PassEndReason::kBufferAndImageBarriers), reopens_total / f,
            s.pass_reopens_after_resolve / f,
            reopens(PassEndReason::kBufferBarriers),
            reopens(PassEndReason::kImageBarriers),
            reopens(PassEndReason::kBufferAndImageBarriers),
            reopens(PassEndReason::kSubmission));
        auto origins = [&](PassEndReason reason) {
          return s.pass_ending_barrier_origins[size_t(reason)] / f;
        };
        XELOGI(
            "VkPassEndBarriers: per frame, passes ended by barriers pushed "
            "while: textures={:.1f} shared_memory={:.1f} render_targets={:.1f} "
            "bindings={:.1f} primitives={:.1f} resolve={:.1f} query={:.1f} "
            "other={:.1f}",
            origins(PassEndReason::kTextures),
            origins(PassEndReason::kSharedMemory),
            origins(PassEndReason::kRenderTargets),
            origins(PassEndReason::kBindings),
            origins(PassEndReason::kPrimitiveProcessor),
            origins(PassEndReason::kResolve), origins(PassEndReason::kQuery),
            origins(PassEndReason::kOther));
      }
      // Ownership transfers of the draws, by source -> destination.
      {
        std::string transfer_stats = render_target_cache_->TakeTransferStats(f);
        if (!transfer_stats.empty()) {
          XELOGI("VkXfer: {}", transfer_stats);
        }
        if (s.zpd_transfer_suspends) {
          XELOGI(
              "VkZpd: per frame, occlusion query segments closed around "
              "in-pass transfers: {:.1f}",
              s.zpd_transfer_suspends / f);
        }
      }
      // Per-render-pass-bucket GPU time (key: WxH, bit31 = ownership transfer).
      if (!pass_bucket_stats_.empty()) {
        for (const auto& kv : pass_bucket_stats_) {
          const uint32_t key = kv.first;
          if (key & kMiscTimestampKeyBit) {
            // Work outside passes and resolves, logged below.
            continue;
          }
          const bool transfer = (key & 0x80000000u) != 0;
          XELOGI(
              "VkPassTime: {}{}x{} : {:.2f}ms/fr ({:.1f}pass {:.0f}draw/fr, "
              "{:.3f}ms ea) scissor<={}x{} viewport<={}x{}",
              transfer ? "xfer " : "", (key >> 16) & 0x7FFF, key & 0xFFFF,
              kv.second.ns / f / 1e6, kv.second.passes / f, kv.second.draws / f,
              kv.second.passes
                  ? kv.second.ns / static_cast<double>(kv.second.passes) / 1e6
                  : 0.0,
              kv.second.max_scissor_w, kv.second.max_scissor_h,
              kv.second.max_viewport_w, kv.second.max_viewport_h);
        }
        // GPU work outside passes and resolves (MiscTimestampKind), such as
        // texture loads by format, size (texels, rounded up to a power of 2)
        // and source (gpu - written by a resolve, cpu - by the guest CPU).
        for (const auto& kv : pass_bucket_stats_) {
          const uint32_t key = kv.first;
          if (!(key & kMiscTimestampKeyBit)) {
            continue;
          }
          std::string label;
          switch (MiscTimestampKind((key >> 24) & 0x3F)) {
            case MiscTimestampKind::kTextureLoad:
              label = fmt::format(
                  "{} {} 2^{}tx {}{}{}",
                  (key & kMiscTimestampTextureCopy) ? "texcopy" : "texload",
                  FormatInfo::GetName(xenos::TextureFormat((key >> 16) & 0x3F)),
                  (key >> 8) & 0x1F,
                  (key & kMiscTimestampTextureGpuWritten) ? "gpu" : "cpu",
                  (key & kMiscTimestampTextureBase) ? "" : " mips-only",
                  (key & (kMiscTimestampTextureBase |
                          kMiscTimestampTextureMips)) ==
                          (kMiscTimestampTextureBase |
                           kMiscTimestampTextureMips)
                      ? "+mips"
                      : "");
              break;
            case MiscTimestampKind::kSetupCommands:
              label = "setup (shared memory uploads)";
              break;
            case MiscTimestampKind::kResolveCopyDispatch: {
              const bool depth = (key & (1u << 16)) != 0;
              const uint32_t format = (key >> 19) & 0xF;
              label = fmt::format(
                  "resolve dispatch {}{} {}x{} {} {}x",
                  depth ? "depth" : "color",
                  (key & (1u << 23)) ? " direct" : "", ((key >> 8) & 0xFF) * 8,
                  (key & 0xFF) * 8,
                  depth ? xenos::GetDepthRenderTargetFormatName(
                              xenos::DepthRenderTargetFormat(format))
                        : xenos::GetColorRenderTargetFormatName(
                              xenos::ColorRenderTargetFormat(format)),
                  1u << ((key >> 17) & 0x3));
            } break;
            default:
              label = fmt::format("misc {:08X}", key);
              break;
          }
          XELOGI("VkMiscTime: {} : {:.2f}ms/fr ({:.1f}/fr, {:.3f}ms ea)",
                 label, kv.second.ns / f / 1e6, kv.second.passes / f,
                 kv.second.passes ? kv.second.ns /
                                        static_cast<double>(kv.second.passes) /
                                        1e6
                                  : 0.0);
        }
        pass_bucket_stats_.clear();
      }
      if (pass_ts_dropped_) {
        XELOGI("VkPassTime: {} pass pairs dropped (raise pool)",
               pass_ts_dropped_);
        pass_ts_dropped_ = 0;
      }
      // Per-resolve GPU time by kind and size (see
      // VulkanRenderTargetCache::last_resolve_key).
      for (const auto& kv : resolve_bucket_stats_) {
        const uint32_t key = kv.first;
        const char* kind = (key & (1u << 29))
                               ? ((key & (1u << 30)) ? "depth" : "color")
                               : "none";
        // Source format and MSAA of the copy.
        std::string source;
        if (key & (1u << 29)) {
          const uint32_t format = (key >> 24) & 0xF;
          source = fmt::format(
              " {} {}x",
              (key & (1u << 30))
                  ? xenos::GetDepthRenderTargetFormatName(
                        xenos::DepthRenderTargetFormat(format))
                  : xenos::GetColorRenderTargetFormatName(
                        xenos::ColorRenderTargetFormat(format)),
              1u << ((key >> 22) & 0x3));
        }
        XELOGI(
            "VkResolveTime: copy={}{}{} {}x{}{} : {:.2f}ms/fr ({:.1f}/fr, "
            "{:.3f}ms ea, max {:.3f}) | copy {:.2f}ms/fr clear {:.2f}ms/fr",
            kind, (key & (1u << 31)) ? "+clear" : "",
            (key & (1u << 28)) ? " direct" : "", ((key >> 11) & 0x7FF) * 8,
            (key & 0x7FF) * 8, source, kv.second.ns / f / 1e6,
            kv.second.count / f,
            kv.second.count
                ? kv.second.ns / static_cast<double>(kv.second.count) / 1e6
                : 0.0,
            kv.second.max_ns / 1e6, kv.second.copy_ns / f / 1e6,
            kv.second.clear_ns / f / 1e6);
      }
      if (await_sites[0].address) {
        Dl_info await_module;
        const uintptr_t module_base =
            dladdr(&await_sites, &await_module)
                ? reinterpret_cast<uintptr_t>(await_module.dli_fbase)
                : 0;
        std::string sites;
        for (AwaitSite& site : await_sites) {
          if (!site.address) {
            break;
          }
          sites += fmt::format(" +{:X} {:.2f}/fr {:.1f}ms/fr;",
                               site.address - module_base, site.count / f,
                               site.ns / f / 1e6);
          site = AwaitSite();
        }
        XELOGI("VkAwaitSites:{}", sites);
      }
      if (barrier_sites[0].address) {
        Dl_info barrier_module;
        const uintptr_t module_base =
            dladdr(&barrier_sites, &barrier_module)
                ? reinterpret_cast<uintptr_t>(barrier_module.dli_fbase)
                : 0;
        std::string sites;
        for (BarrierSite& site : barrier_sites) {
          if (!site.address) {
            break;
          }
          sites += fmt::format(" +{:X} {:.1f}/fr {:.1f}b;",
                               site.address - module_base, site.calls / f,
                               site.barriers / f);
          site = BarrierSite();
        }
        XELOGI("VkBarrierSites:{}", sites);
        std::string stages;
        for (BarrierStages& stage : barrier_stages) {
          if (!stage.count) {
            break;
          }
          stages += fmt::format(" {:X}->{:X} {:.1f}/fr;", stage.src, stage.dst,
                                stage.count / f);
          stage = BarrierStages();
        }
        XELOGI("VkBarrierStages:{}", stages);
      }
      resolve_bucket_stats_.clear();
      s = VkFrameSyncStats();
      s.last_report_ns = now;
    }
  }

  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    return;
  }

  // Obtaining the actual front buffer size to pass to RefreshGuestOutput,
  // resolution-scaled if it's a resolve destination, or not otherwise.
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  xenos::TextureFormat frontbuffer_format;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format);
  if (swap_texture_view == VK_NULL_HANDLE) {
    return;
  }

  auto aspect = graphics_system_->GetScaledAspectRatio();

  presenter->RefreshGuestOutput(
      frontbuffer_width_scaled, frontbuffer_height_scaled, aspect.first,
      aspect.second,
      [this, frontbuffer_width_scaled, frontbuffer_height_scaled,
       frontbuffer_format, swap_texture_view](
          ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context = static_cast<
            ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
            context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn =
            vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index =
            uint32_t(frame_current_ % kMaxFramesInFlight);

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format ==
                xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_image_ != VK_NULL_HANDLE &&
              (fxaa_source_width_ != frontbuffer_width_scaled ||
               fxaa_source_height_ != frontbuffer_height_scaled)) {
            // Need to resize the FXAA source texture.
            if (GetCompletedSubmission() < fxaa_source_last_submission_) {
              // Still in use - defer destruction.
              destroy_memory_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_memory_);
              destroy_images_.emplace_back(fxaa_source_last_submission_,
                                           fxaa_source_image_);
              destroy_image_views_.emplace_back(fxaa_source_last_submission_,
                                                fxaa_source_image_view_);
            } else {
              dfn.vkDestroyImageView(device, fxaa_source_image_view_, nullptr);
              dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
              dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
            }
            fxaa_source_image_ = VK_NULL_HANDLE;
            fxaa_source_image_view_ = VK_NULL_HANDLE;
            fxaa_source_memory_ = VK_NULL_HANDLE;
            fxaa_source_width_ = 0;
            fxaa_source_height_ = 0;
            fxaa_source_last_submission_ = 0;
          }
          if (fxaa_source_image_ == VK_NULL_HANDLE) {
            // Create the FXAA source texture.
            VkImageCreateInfo fxaa_source_image_create_info;
            fxaa_source_image_create_info.sType =
                VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            fxaa_source_image_create_info.pNext = nullptr;
            fxaa_source_image_create_info.flags = 0;
            fxaa_source_image_create_info.imageType = VK_IMAGE_TYPE_2D;
            fxaa_source_image_create_info.format = kFxaaSourceFormat;
            fxaa_source_image_create_info.extent.width =
                frontbuffer_width_scaled;
            fxaa_source_image_create_info.extent.height =
                frontbuffer_height_scaled;
            fxaa_source_image_create_info.extent.depth = 1;
            fxaa_source_image_create_info.mipLevels = 1;
            fxaa_source_image_create_info.arrayLayers = 1;
            fxaa_source_image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
            fxaa_source_image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            fxaa_source_image_create_info.usage =
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            fxaa_source_image_create_info.sharingMode =
                VK_SHARING_MODE_EXCLUSIVE;
            fxaa_source_image_create_info.queueFamilyIndexCount = 0;
            fxaa_source_image_create_info.pQueueFamilyIndices = nullptr;
            fxaa_source_image_create_info.initialLayout =
                VK_IMAGE_LAYOUT_UNDEFINED;
            if (dfn.vkCreateImage(device, &fxaa_source_image_create_info,
                                  nullptr, &fxaa_source_image_) != VK_SUCCESS) {
              XELOGE("Failed to create the FXAA source image");
              use_fxaa = false;
            } else {
              VkMemoryRequirements fxaa_source_memory_requirements;
              dfn.vkGetImageMemoryRequirements(
                  device, fxaa_source_image_, &fxaa_source_memory_requirements);
              VkMemoryAllocateInfo fxaa_source_memory_allocate_info;
              fxaa_source_memory_allocate_info.sType =
                  VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
              fxaa_source_memory_allocate_info.pNext = nullptr;
              fxaa_source_memory_allocate_info.allocationSize =
                  fxaa_source_memory_requirements.size;
              fxaa_source_memory_allocate_info.memoryTypeIndex =
                  ui::vulkan::util::ChooseMemoryType(
                      vulkan_device->memory_types(),
                      fxaa_source_memory_requirements.memoryTypeBits,
                      ui::vulkan::util::MemoryPurpose::kDeviceLocal);
              if (fxaa_source_memory_allocate_info.memoryTypeIndex ==
                      UINT32_MAX ||
                  dfn.vkAllocateMemory(
                      device, &fxaa_source_memory_allocate_info, nullptr,
                      &fxaa_source_memory_) != VK_SUCCESS) {
                XELOGE("Failed to allocate FXAA source image memory");
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else if (dfn.vkBindImageMemory(device, fxaa_source_image_,
                                               fxaa_source_memory_,
                                               0) != VK_SUCCESS) {
                XELOGE("Failed to bind FXAA source image memory");
                dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                fxaa_source_memory_ = VK_NULL_HANDLE;
                dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                fxaa_source_image_ = VK_NULL_HANDLE;
                use_fxaa = false;
              } else {
                VkImageViewCreateInfo fxaa_source_image_view_create_info;
                fxaa_source_image_view_create_info.sType =
                    VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                fxaa_source_image_view_create_info.pNext = nullptr;
                fxaa_source_image_view_create_info.flags = 0;
                fxaa_source_image_view_create_info.image = fxaa_source_image_;
                fxaa_source_image_view_create_info.viewType =
                    VK_IMAGE_VIEW_TYPE_2D;
                fxaa_source_image_view_create_info.format = kFxaaSourceFormat;
                fxaa_source_image_view_create_info.components.r =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.g =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.b =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.components.a =
                    VK_COMPONENT_SWIZZLE_IDENTITY;
                fxaa_source_image_view_create_info.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseMipLevel = 0;
                fxaa_source_image_view_create_info.subresourceRange.levelCount =
                    1;
                fxaa_source_image_view_create_info.subresourceRange
                    .baseArrayLayer = 0;
                fxaa_source_image_view_create_info.subresourceRange.layerCount =
                    1;
                if (dfn.vkCreateImageView(
                        device, &fxaa_source_image_view_create_info, nullptr,
                        &fxaa_source_image_view_) != VK_SUCCESS) {
                  XELOGE("Failed to create the FXAA source image view");
                  dfn.vkFreeMemory(device, fxaa_source_memory_, nullptr);
                  fxaa_source_memory_ = VK_NULL_HANDLE;
                  dfn.vkDestroyImage(device, fxaa_source_image_, nullptr);
                  fxaa_source_image_ = VK_NULL_HANDLE;
                  use_fxaa = false;
                } else {
                  fxaa_source_width_ = frontbuffer_width_scaled;
                  fxaa_source_height_ = frontbuffer_height_scaled;
                }
              }
            }
          }
        }

        // FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref =
            use_pwl_gamma_ramp ? gamma_ramp_pwl_current_frame_
                               : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable =
              sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize =
              kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size = use_pwl_gamma_ramp
                                         ? kGammaRampSizePWL
                                         : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) +
              gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little &&
              use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                    gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry =
                  gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(
                gamma_ramp_frame_upload,
                use_pwl_gamma_ramp
                    ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                    : static_cast<const void*>(gamma_ramp_256_entry_table()),
                gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer =
              gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            InsertDebugMarker("Gamma Ramp Upload: %u bytes",
                              static_cast<uint32_t>(gamma_ramp_size));
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_,
                                                     gamma_ramp_buffer_, 1,
                                                     &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(
                gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref =
              gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        // Track FXAA source texture submission.
        if (use_fxaa) {
          fxaa_source_last_submission_ = GetCurrentSubmission();
        }

        // Determine the destination image for gamma application.
        // If FXAA is enabled, gamma writes to the FXAA source texture.
        // Otherwise, it writes directly to the guest output.
        VkImage apply_gamma_dest_image =
            use_fxaa ? fxaa_source_image_ : vulkan_context.image();
        VkImageView apply_gamma_dest_image_view =
            use_fxaa ? fxaa_source_image_view_ : vulkan_context.image_view();

        // Transition destination image for compute shader storage image write.
        if (use_fxaa) {
          // FXAA source - transition from undefined (we'll be overwriting).
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        } else if (vulkan_context.image_ever_written_previously()) {
          // Guest output - insert a barrier after the last presenter's usage.
          // Will be overwriting all the contents, so oldLayout can be
          // UNDEFINED.
          PushImageMemoryBarrier(
              apply_gamma_dest_image,
              ui::vulkan::util::InitializeSubresourceRange(),
              ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
              VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL);
        } else {
          // Guest output - first write to the image, just transition.
          PushImageMemoryBarrier(apply_gamma_dest_image,
                                 ui::vulkan::util::InitializeSubresourceRange(),
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
        }

        // End the current render pass before inserting barriers.
        SubmitBarriers(true);

        PushDebugMarker("Apply Gamma Ramp: %s%s",
                        use_pwl_gamma_ramp ? "PWL" : "256-entry table",
                        use_fxaa ? " (with FXAA luma)" : "");

        // Select the appropriate gamma pipeline.
        VkPipeline gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          gamma_pipeline = use_fxaa ? swap_apply_gamma_pwl_fxaa_luma_pipeline_
                                    : swap_apply_gamma_pwl_pipeline_;
        } else {
          gamma_pipeline =
              use_fxaa ? swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_
                       : swap_apply_gamma_256_entry_table_pipeline_;
        }
        deferred_command_buffer_.CmdVkBindPipeline(
            VK_PIPELINE_BIND_POINT_COMPUTE, gamma_pipeline);

        // Update the source descriptor set with the swap texture.
        VkDescriptorSet swap_descriptor_source =
            swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType =
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo =
            &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;

        // Update the destination descriptor set with the destination image.
        // When FXAA is enabled, use a separate descriptor set for writing to
        // FXAA source to avoid the issue where both passes would use the same
        // descriptor set (which gets updated twice before submission).
        VkDescriptorSet swap_descriptor_dest =
            use_fxaa ? fxaa_source_storage_descriptors_[swap_frame_index]
                     : swap_descriptors_dest_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_dest_image_info;
        swap_descriptor_dest_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_dest_image_info.imageView = apply_gamma_dest_image_view;
        swap_descriptor_dest_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet swap_descriptor_dest_write;
        swap_descriptor_dest_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_dest_write.pNext = nullptr;
        swap_descriptor_dest_write.dstSet = swap_descriptor_dest;
        swap_descriptor_dest_write.dstBinding = 0;
        swap_descriptor_dest_write.dstArrayElement = 0;
        swap_descriptor_dest_write.descriptorCount = 1;
        swap_descriptor_dest_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        swap_descriptor_dest_write.pImageInfo =
            &swap_descriptor_dest_image_info;
        swap_descriptor_dest_write.pBufferInfo = nullptr;
        swap_descriptor_dest_write.pTexelBufferView = nullptr;

        std::array<VkWriteDescriptorSet, 2> swap_descriptor_writes = {
            swap_descriptor_source_write, swap_descriptor_dest_write};
        dfn.vkUpdateDescriptorSets(device,
                                   uint32_t(swap_descriptor_writes.size()),
                                   swap_descriptor_writes.data(), 0, nullptr);

        // Set push constants.
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = frontbuffer_width_scaled;
        apply_gamma_constants.size[1] = frontbuffer_height_scaled;
        deferred_command_buffer_.CmdVkPushConstants(
            swap_apply_gamma_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(apply_gamma_constants), &apply_gamma_constants);

        // Bind descriptor sets.
        std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount>
            swap_descriptor_sets{};
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
            swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                         uint32_t(use_pwl_gamma_ramp)];
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] =
            swap_descriptor_source;
        swap_descriptor_sets[kSwapApplyGammaDescriptorSetDest] =
            swap_descriptor_dest;
        // TODO(Triang3l): Red / blue swap without imageViewFormatSwizzle.
        deferred_command_buffer_.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_pipeline_layout_,
            0, uint32_t(swap_descriptor_sets.size()),
            swap_descriptor_sets.data(), 0, nullptr);

        // Dispatch compute shader. Local size is 16x8.
        uint32_t group_count_x = (frontbuffer_width_scaled + 15) / 16;
        uint32_t group_count_y = (frontbuffer_height_scaled + 7) / 8;
        ++submission_in_progress_.dispatch_count;
        deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

        PopDebugMarker();

        // Apply FXAA if enabled.
        if (use_fxaa) {
          PushDebugMarker("FXAA: %s",
                          swap_post_effect == SwapPostEffect::kFxaaExtreme
                              ? "Extreme"
                              : "Standard");

          // Transition FXAA source from storage image write to sampled read.
          // Transition guest output to storage image write.
          PushImageMemoryBarrier(
              fxaa_source_image_,
              ui::vulkan::util::InitializeSubresourceRange(),
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
          if (vulkan_context.image_ever_written_previously()) {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          } else {
            PushImageMemoryBarrier(
                vulkan_context.image(),
                ui::vulkan::util::InitializeSubresourceRange(),
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);

          // Bind FXAA pipeline.
          deferred_command_buffer_.CmdVkBindPipeline(
              VK_PIPELINE_BIND_POINT_COMPUTE,
              swap_post_effect == SwapPostEffect::kFxaaExtreme
                  ? fxaa_extreme_pipeline_
                  : fxaa_pipeline_);

          // Update FXAA source descriptor (combined image sampler).
          VkDescriptorSet fxaa_source_descriptor =
              fxaa_source_descriptors_[swap_frame_index];
          VkDescriptorImageInfo fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_image_info.sampler = fxaa_sampler_;
          fxaa_source_descriptor_image_info.imageView = fxaa_source_image_view_;
          fxaa_source_descriptor_image_info.imageLayout =
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          VkWriteDescriptorSet fxaa_source_descriptor_write;
          fxaa_source_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_source_descriptor_write.pNext = nullptr;
          fxaa_source_descriptor_write.dstSet = fxaa_source_descriptor;
          fxaa_source_descriptor_write.dstBinding = 0;
          fxaa_source_descriptor_write.dstArrayElement = 0;
          fxaa_source_descriptor_write.descriptorCount = 1;
          fxaa_source_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          fxaa_source_descriptor_write.pImageInfo =
              &fxaa_source_descriptor_image_info;
          fxaa_source_descriptor_write.pBufferInfo = nullptr;
          fxaa_source_descriptor_write.pTexelBufferView = nullptr;

          // Update FXAA destination descriptor (guest output image).
          VkDescriptorSet fxaa_dest_descriptor =
              swap_descriptors_dest_[swap_frame_index];
          VkDescriptorImageInfo fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_image_info.sampler = VK_NULL_HANDLE;
          fxaa_dest_descriptor_image_info.imageView =
              vulkan_context.image_view();
          fxaa_dest_descriptor_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet fxaa_dest_descriptor_write;
          fxaa_dest_descriptor_write.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          fxaa_dest_descriptor_write.pNext = nullptr;
          fxaa_dest_descriptor_write.dstSet = fxaa_dest_descriptor;
          fxaa_dest_descriptor_write.dstBinding = 0;
          fxaa_dest_descriptor_write.dstArrayElement = 0;
          fxaa_dest_descriptor_write.descriptorCount = 1;
          fxaa_dest_descriptor_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          fxaa_dest_descriptor_write.pImageInfo =
              &fxaa_dest_descriptor_image_info;
          fxaa_dest_descriptor_write.pBufferInfo = nullptr;
          fxaa_dest_descriptor_write.pTexelBufferView = nullptr;

          std::array<VkWriteDescriptorSet, 2> fxaa_descriptor_writes = {
              fxaa_source_descriptor_write, fxaa_dest_descriptor_write};
          dfn.vkUpdateDescriptorSets(device,
                                     uint32_t(fxaa_descriptor_writes.size()),
                                     fxaa_descriptor_writes.data(), 0, nullptr);

          // Set FXAA push constants.
          FxaaConstants fxaa_constants;
          fxaa_constants.size[0] = frontbuffer_width_scaled;
          fxaa_constants.size[1] = frontbuffer_height_scaled;
          fxaa_constants.size_inv[0] =
              1.0f / static_cast<float>(frontbuffer_width_scaled);
          fxaa_constants.size_inv[1] =
              1.0f / static_cast<float>(frontbuffer_height_scaled);
          deferred_command_buffer_.CmdVkPushConstants(
              fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(fxaa_constants), &fxaa_constants);

          // Bind FXAA descriptor sets.
          std::array<VkDescriptorSet, 2> fxaa_descriptor_sets = {
              fxaa_dest_descriptor, fxaa_source_descriptor};
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, fxaa_pipeline_layout_, 0,
              uint32_t(fxaa_descriptor_sets.size()),
              fxaa_descriptor_sets.data(), 0, nullptr);

          // Dispatch FXAA compute shader.
          ++submission_in_progress_.dispatch_count;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y,
                                                 1);

          PopDebugMarker();
        }

        // Insert the release barrier - transition from GENERAL to the
        // presenter's expected layout.
        PushImageMemoryBarrier(
            vulkan_context.image(),
            ui::vulkan::util::InitializeSubresourceRange(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
            VK_ACCESS_SHADER_WRITE_BIT,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
            VK_IMAGE_LAYOUT_GENERAL,
            ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  // Pump any completed resolves now since the guest is likely about to poll.
  PumpQueryResolves();
  PumpPendingRetire();

  if (cvars::submit_on_primary_buffer_end && submission_open_ &&
      !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
    ++vk_frame_sync_stats_.primary_buffer_splits;
    EndSubmission(false);
  }
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE &&
         offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset +
                 other_buffer_memory_barrier.size <=
             offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset &&
        other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  pending_barrier_origins_ |= uint32_t(1) << uint32_t(pass_end_reason_);
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask,
    VkImageLayout old_layout, VkImageLayout new_layout,
    uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask &&
      src_access_mask == dst_access_mask && old_layout == new_layout &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask &
          subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount !=
             VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount !=
             VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask ==
            subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount ==
            subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount ==
            subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex ==
            src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex ==
            dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  pending_barrier_origins_ |= uint32_t(1) << uint32_t(pass_end_reason_);
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  void* const barrier_caller = __builtin_return_address(0);
  assert_true(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      EndRenderPass();
    }
    return false;
  }
  if (cvars::log_gpu_frame_time_breakdown) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(barrier_caller);
    for (BarrierSite& site : barrier_sites) {
      if (!site.address || site.address == address) {
        site.address = address;
        ++site.calls;
        site.barriers += pending_barriers_.size();
        break;
      }
    }
    for (const auto& pending : pending_barriers_) {
      for (BarrierStages& stages : barrier_stages) {
        if (!stages.count || (stages.src == pending.src_stage_mask &&
                              stages.dst == pending.dst_stage_mask)) {
          stages.src = pending.src_stage_mask;
          stages.dst = pending.dst_stage_mask;
          ++stages.count;
          break;
        }
      }
    }
  }
  if (in_render_pass_ && cvars::log_gpu_frame_time_breakdown) {
    // Who pushed the barriers that end the pass (the PassEndReason scope open
    // when each was pushed).
    uint32_t origins = pending_barrier_origins_;
    uint32_t origin;
    while (xe::bit_scan_forward(origins, &origin)) {
      origins &= ~(uint32_t(1) << origin);
      ++vk_frame_sync_stats_.pass_ending_barrier_origins[origin];
    }
    // The barriers are what ends the pass here (VkPassEnd).
    const bool buffer_barriers =
        !pending_barriers_buffer_memory_barriers_.empty();
    const bool image_barriers = !pending_barriers_image_memory_barriers_.empty();
    PassEndReasonScope pass_end_reason_scope(
        *this, buffer_barriers
                   ? (image_barriers ? PassEndReason::kBufferAndImageBarriers
                                     : PassEndReason::kBufferBarriers)
                   : (image_barriers ? PassEndReason::kImageBarriers
                                     : pass_end_reason_));
    EndRenderPass();
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend();
       ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask
                           : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask
                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() +
            it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() +
            it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  pending_barrier_origins_ = 0;
  return true;
}

void VulkanCommandProcessor::OpenPassTimestamp(uint32_t bucket_key) {
  if (!pass_timestamp_mapping_) {
    return;
  }
  if (pass_ts_submission_ != GetCurrentSubmission()) {
    pass_ts_submission_ = GetCurrentSubmission();
    pass_ts_count_ = 0;
  }
  if (pass_ts_count_ >= kPassTimestampPairsPerSubmission) {
    ++pass_ts_dropped_;
    return;
  }
  const uint32_t pair =
      uint32_t(pass_ts_submission_ % kPassTimestampRingSubmissions) *
          kPassTimestampPairsPerSubmission +
      pass_ts_count_;
  pass_ts_keys_[pair] = bucket_key;
  deferred_command_buffer_.CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timestamp_pool_, pair * 2);
  pass_ts_open_pair_ = pair;
  pass_ts_open_submission_ = pass_ts_submission_;
  pass_open_draws_ = 0;
  pass_open_scissor_w_ = 0;
  pass_open_scissor_h_ = 0;
  pass_open_viewport_w_ = 0;
  pass_open_viewport_h_ = 0;
}

void VulkanCommandProcessor::ClosePassTimestamp() {
  if (pass_ts_open_pair_ == UINT32_MAX) {
    return;
  }
  // A pass that spanned a submission split has its begin in a prior
  // submission's already-copied range; abandon it rather than corrupt this
  // submission's pair count (its begin query is simply never copied).
  if (pass_ts_open_submission_ != GetCurrentSubmission()) {
    pass_ts_open_pair_ = UINT32_MAX;
    ++pass_ts_dropped_;
    return;
  }
  deferred_command_buffer_.CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pass_timestamp_pool_,
      pass_ts_open_pair_ * 2 + 1);
  pass_ts_draws_[pass_ts_open_pair_] = pass_open_draws_;
  pass_ts_scissor_[pass_ts_open_pair_] =
      (std::min(pass_open_scissor_w_, 0xFFFFu) << 16) |
      std::min(pass_open_scissor_h_, 0xFFFFu);
  pass_ts_viewport_[pass_ts_open_pair_] =
      (std::min(pass_open_viewport_w_, 0xFFFFu) << 16) |
      std::min(pass_open_viewport_h_, 0xFFFFu);
  ++pass_ts_count_;
  pass_ts_open_pair_ = UINT32_MAX;
}

bool VulkanCommandProcessor::OpenMiscTimestamp(uint32_t key) {
  // One open pair at a time in the ring - never while a pass is being timed.
  if (!pass_timestamp_mapping_ || in_render_pass_ ||
      pass_ts_open_pair_ != UINT32_MAX) {
    return false;
  }
  OpenPassTimestamp(key | kMiscTimestampKeyBit);
  return pass_ts_open_pair_ != UINT32_MAX;
}

void VulkanCommandProcessor::CloseMiscTimestamp() {
  if (pass_ts_open_pair_ == UINT32_MAX || in_render_pass_ ||
      !(pass_ts_keys_[pass_ts_open_pair_] & kMiscTimestampKeyBit)) {
    return;
  }
  ClosePassTimestamp();
}

void VulkanCommandProcessor::MarkResolveCopyEnd() {
  if (resolve_ts_open_ == UINT32_MAX || resolve_ts_copy_end_written_) {
    return;
  }
  deferred_command_buffer_.CmdVkWriteTimestamp(
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resolve_timestamp_pool_,
      resolve_ts_open_ * kResolveTimestampsPerResolve + 1);
  resolve_ts_copy_end_written_ = true;
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass &&
        current_framebuffer_ == framebuffer) {
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is. Also closes the fork's pass timestamp.
  {
    PassEndReasonScope pass_end_reason_scope(*this,
                                             PassEndReason::kRenderTargets);
    EndRenderPass();
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;
  ++vk_frame_sync_stats_.render_pass_begins;
  if (cvars::log_gpu_frame_time_breakdown &&
      framebuffer == last_ended_framebuffer_) {
    ++vk_frame_sync_stats_.pass_reopens[size_t(last_pass_end_reason_)];
    vk_frame_sync_stats_.pass_reopens_after_resolve +=
        uint64_t(resolve_since_pass_end_);
  }
  BinTraceNoteIfActive("P", framebuffer->host_extent.width,
                       framebuffer->host_extent.height);
  // Identify each pass bucket once by the guest render targets behind it.
  if (cvars::log_gpu_frame_time_breakdown) {
    const uint32_t bucket = (uint32_t(framebuffer->host_extent.width) << 16) |
                            uint32_t(framebuffer->host_extent.height);
    static std::set<uint32_t> logged_buckets;
    if (logged_buckets.emplace(bucket).second) {
      XELOGI("VkPassId: {}x{} <- {}", framebuffer->host_extent.width,
             framebuffer->host_extent.height,
             render_target_cache_->GetLastUpdateRenderTargetsDebugName());
    }
  }
  OpenPassTimestamp((uint32_t(framebuffer->host_extent.width) << 16) |
                    framebuffer->host_extent.height);

  if (use_dynamic_rendering) {
    // Use dynamic rendering - construct VkRenderingInfo from render targets.
    // The PSI ("accuracy") path emulates EDRAM via SSBOs and uses no real
    // attachments (mirrors the zero-attachment fsi_render_pass_).
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};
    uint32_t color_attachment_count = 0;

    bool has_depth = false;
    bool has_stencil = false;
    if (render_target_cache_->GetPath() ==
        RenderTargetCache::Path::kHostRenderTargets) {
      render_target_cache_->GetLastUpdateRenderingAttachments(
          color_attachments, &color_attachment_count, &depth_attachment,
          &stencil_attachment);
      has_depth =
          depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      has_stencil = stencil_attachment.sType ==
                    VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments =
        color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        has_stencil ? &stencil_attachment : nullptr;

    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;

  // Resume any pending ZPD segment now that the pass is open.
  OpenQuerySegment(false);
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass,
    const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth) {
  SCOPE_profile_cpu_f("gpu");
  SubmitBarriers(false);

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering = cvars::vulkan_dynamic_rendering &&
                               vulkan_device->properties().dynamicRendering;

  // Check if we can stay in the current render pass.
  if (use_dynamic_rendering) {
    // For dynamic rendering, compare framebuffer directly.
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass &&
        current_framebuffer_ == framebuffer) {
      return;
    }
  }

  // End current render pass/rendering if active, via EndRenderPass so any open
  // occlusion query segment is closed first. A query begun inside the pass must
  // be ended before the pass is. Also closes the fork's pass timestamp.
  {
    PassEndReasonScope pass_end_reason_scope(*this,
                                             PassEndReason::kRenderTargets);
    EndRenderPass();
  }

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;
  ++vk_frame_sync_stats_.render_pass_begins;
  BinTraceNoteIfActive(transfer_dest_is_depth ? "XD" : "XC",
                       framebuffer->host_extent.width,
                       framebuffer->host_extent.height);
  // Bit 31 tags EDRAM ownership-transfer passes.
  OpenPassTimestamp(0x80000000u |
                    (uint32_t(framebuffer->host_extent.width) << 16) |
                    framebuffer->host_extent.height);

  if (use_dynamic_rendering) {
    // Use dynamic rendering for transfers - construct VkRenderingInfo from
    // the transfer destination.
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout =
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      // Stencil uses the same attachment.
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      // Must agree with the layout the usage barrier actually recorded, which
      // is RENDERING_LOCAL_READ when local-read attachments are enabled.
      color_attachment.imageLayout = render_target_cache_->color_draw_layout();
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments =
        transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment =
        transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment =
        transfer_dest_is_depth ? &stencil_attachment : nullptr;

    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    // Use traditional render pass.
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;

  OpenQuerySegment(false);
}

void VulkanCommandProcessor::EndRenderPass() {
  assert_true(submission_open_);
  if (!in_render_pass_) {
    return;
  }
  if (cvars::log_gpu_frame_time_breakdown) {
    ++vk_frame_sync_stats_.pass_ends[size_t(pass_end_reason_)];
    last_ended_framebuffer_ = current_framebuffer_;
    last_pass_end_reason_ = pass_end_reason_;
    resolve_since_pass_end_ = false;
  }
  // Close native Vulkan occlusion queries before ending the pass. FSI counter
  // segments don't use vkCmdBeginQuery / vkCmdEndQuery and can stay logically
  // open across render passes.
  if (GetZPDMode() != ZPDMode::kFake && zpd_active_segment_.segment_active &&
      !zpd_active_query_is_fsi_) {
    CloseQuerySegment();
    if (zpd_active_segment_.logical_active) {
      zpd_active_segment_.segment_pending_begin = true;
    }
  }
  // Shrink the render area to what the pass's draws actually touched, now that
  // they have all been recorded. A host render target spans the whole EDRAM
  // range for its pitch (a 2-tile-wide one is 8192 rows tall), and on a tiler
  // the render area drives tile binning and GMEM load/store - not just
  // clipping - so the unshrunk area costs far more than the draws do.
  if (cvars::render_area_dirty_extent) {
    // Rounded up to a coarse bin-friendly quantum rather than the exact
    // vkGetRenderAreaGranularity: a non-aligned render area is legal, the
    // alignment only helps the driver's binning, and the win here is 8192 rows
    // becoming ~135 - the rounding is noise against that.
    constexpr uint32_t kRenderAreaAlign = 32;
    deferred_command_buffer_.ShrinkRenderAreaToDrawn(kRenderAreaAlign,
                                                     kRenderAreaAlign);
  }
  // Use current_render_pass_ to determine which end command to use.
  // VK_NULL_HANDLE means we used dynamic rendering, otherwise traditional.
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRendering();
  } else {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
  ClosePassTimestamp();
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    bool is_storage_buffer =
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageBufferFragment;
    bool is_storage_image =
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageImageFragment ||
        transient_descriptor_layout ==
            SingleTransientDescriptorLayout::kStorageImageCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator&
        transient_descriptor_allocator =
            is_storage_image
                ? transient_descriptor_allocator_storage_image_
                : (is_storage_buffer
                       ? transient_descriptor_allocator_storage_buffer_
                       : transient_descriptor_allocator_uniform_buffer_);
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type =
        is_storage_image
            ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
            : (is_storage_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    descriptor_count.descriptorCount = 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout),
        &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(
    bool is_vertex, size_t texture_count, size_t sampler_count) {
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing =
      descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType =
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings =
      descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider*
VulkanCommandProcessor::GetPipelineLayout(size_t texture_count_pixel,
                                          size_t sampler_count_pixel,
                                          size_t texture_count_vertex,
                                          size_t sampler_count_vertex) {
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  // Called from the draw thread and from pipeline creation threads (deferred
  // translation), and reads/writes the shared layout maps +
  // GetTextureDescriptor SetLayout (only called from here), so serialize the
  // whole lookup+create.
  std::lock_guard<std::mutex> layouts_lock(pipeline_layouts_mutex_);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex,
                                    sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel,
                                    sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    XELOGE(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout
      descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts
      [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
          descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags = 0;
  pipeline_layout_create_info.setLayoutCount =
      uint32_t(xe::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr,
                                 &pipeline_layout) != VK_SUCCESS) {
    XELOGE(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(
          pipeline_layout, descriptor_set_layout_textures_vertex,
          descriptor_set_layout_textures_pixel, uint32_t(texture_count_vertex),
          uint32_t(sampler_count_vertex), uint32_t(texture_count_pixel),
          uint32_t(sampler_count_pixel)));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition
VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask,
    VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE,
                            scratch_buffer_last_stage_mask_, initial_stage_mask,
                            scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = xe::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    XELOGE(
        "VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer",
        size >> 20);
    return ScratchBufferAcquisition();
  }

  if (GetCompletedSubmission() >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_,
                                    scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_,
                                   scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_access_mask;
  scratch_buffer_last_access_mask_ = initial_stage_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(
    VkPipeline pipeline, bool keep_dynamic_depth_bias,
    bool keep_dynamic_blend_constants, bool keep_dynamic_stencil_mask_ref) {
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  // External (transfer/resolve) pipelines bake all extended dynamic state
  // statically, invalidating the guest draw's dynamic EDS state. Re-dirty every
  // EDS flag so the next guest draw re-emits it (UpdateDynamicState only emits
  // when the matching capability is active, so this is inert when EDS is off).
  dynamic_cull_mode_update_needed_ = true;
  dynamic_front_face_update_needed_ = true;
  dynamic_primitive_topology_update_needed_ = true;
  dynamic_primitive_restart_enable_update_needed_ = true;
  dynamic_depth_test_enable_update_needed_ = true;
  dynamic_depth_write_enable_update_needed_ = true;
  dynamic_depth_compare_op_update_needed_ = true;
  dynamic_stencil_test_enable_update_needed_ = true;
  dynamic_stencil_op_front_update_needed_ = true;
  dynamic_stencil_op_back_update_needed_ = true;
  dynamic_depth_clamp_enable_update_needed_ = true;
  dynamic_polygon_mode_update_needed_ = true;
  dynamic_color_blend_enable_update_needed_ = true;
  dynamic_color_blend_equation_update_needed_ = true;
  dynamic_color_write_mask_update_needed_ = true;
  dynamic_shading_rate_update_needed_ = true;
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS,
                                             pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = nullptr;
  current_guest_graphics_pipeline_handle_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE,
                                             pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |=
        dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |=
        dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type,
                                           const uint32_t* host_address,
                                           uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::EnsureMemexportSnapshotBuffer(uint32_t size) {
  if (size <= memexport_snapshot_size_) {
    return true;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  // Called with no deferred readback pending, and an exporting draw read back
  // right away awaits its submission before returning, so the previous buffer
  // is idle.
  if (memexport_snapshot_mapping_) {
    dfn.vkUnmapMemory(device, memexport_snapshot_memory_);
    memexport_snapshot_mapping_ = nullptr;
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         memexport_snapshot_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         memexport_snapshot_memory_);
  memexport_snapshot_size_ = 0;
  const uint32_t new_size = xe::align(size, uint32_t(1) << 20);
  uint32_t memory_type = UINT32_MAX;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, new_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback,
          memexport_snapshot_buffer_, memexport_snapshot_memory_,
          &memory_type)) {
    XELOGE("Memexport readback: failed to create a {} KB snapshot buffer",
           new_size >> 10);
    return false;
  }
  void* mapping = nullptr;
  if (dfn.vkMapMemory(device, memexport_snapshot_memory_, 0, VK_WHOLE_SIZE, 0,
                      &mapping) != VK_SUCCESS) {
    XELOGE("Memexport readback: failed to map the snapshot buffer");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                           memexport_snapshot_buffer_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           memexport_snapshot_memory_);
    return false;
  }
  memexport_snapshot_mapping_ = static_cast<const uint8_t*>(mapping);
  memexport_snapshot_coherent_ =
      memory_type < 32 &&
      ((vulkan_device->memory_types().host_coherent >> memory_type) & 1);
  memexport_snapshot_size_ = new_size;
  return true;
}

bool VulkanCommandProcessor::StoreMemexportChanges(uint32_t address,
                                                   uint32_t size,
                                                   const uint8_t* before) {
  memexport_readback_after_.resize(size);
  if (!shared_memory_->ReadHostMapped(address, size,
                                      memexport_readback_after_.data())) {
    XELOGE("Memexport readback failed at {:08X} ({} bytes)", address, size);
    return false;
  }
  const uint8_t* after = memexport_readback_after_.data();
  volatile uint8_t* guest =
      static_cast<volatile uint8_t*>(memory_->TranslatePhysical(address));
  uint32_t changed = 0;
  uint32_t offset = 0;
  for (; offset + 8 <= size; offset += 8) {
    uint64_t before_word, after_word;
    std::memcpy(&before_word, before + offset, sizeof(before_word));
    std::memcpy(&after_word, after + offset, sizeof(after_word));
    if (before_word == after_word) {
      continue;
    }
    for (uint32_t j = offset; j < offset + 8; ++j) {
      if (before[j] != after[j]) {
        guest[j] = after[j];
        ++changed;
      }
    }
  }
  for (; offset < size; ++offset) {
    if (before[offset] != after[offset]) {
      guest[offset] = after[offset];
      ++changed;
    }
  }
  if (changed) {
    shared_memory_->NoteGuestCopy(
        address, size, SharedMemory::GuestCopySource::kMemexportReadback);
  }
  if (cvars::shader_profiling) {
    XELOGI("Memexport readback: {:08X}, {} bytes, {} changed", address, size,
           changed);
  }
  return true;
}

void VulkanCommandProcessor::ApplyCompletedMemexportReadbacks(
    uint64_t completed_submission) {
  // All of them or none: storing into guest memory invalidates the pages, so
  // their next use uploads them from there, over the output of any export
  // still on its way that only its own readback would have stored. With every
  // export done, the GPU's copy read below holds no partial later output
  // either.
  if (pending_memexport_readbacks_.empty() ||
      pending_memexport_readbacks_.back().submission > completed_submission) {
    return;
  }
  // Their submissions ended with a barrier making the writes visible to the
  // host (EndSubmission); a non-coherent snapshot mapping is invalidated.
  if (!memexport_snapshot_coherent_) {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    VkMappedMemoryRange snapshot_range;
    snapshot_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    snapshot_range.pNext = nullptr;
    snapshot_range.memory = memexport_snapshot_memory_;
    snapshot_range.offset = 0;
    snapshot_range.size = VK_WHOLE_SIZE;
    vulkan_device->functions().vkInvalidateMappedMemoryRanges(
        vulkan_device->device(), 1, &snapshot_range);
  }
  // In draw order: with the contents of the GPU's copy now, a byte several
  // exports changed ends up with the last value either way.
  for (const PendingMemexportReadback& pending : pending_memexport_readbacks_) {
    StoreMemexportChanges(pending.address, pending.size,
                          memexport_snapshot_mapping_ + pending.snapshot_offset);
  }
  pending_memexport_readbacks_.clear();
  pending_memexport_snapshot_used_ = 0;
  memexport_readbacks_pending_ = false;
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type,
                                       uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  // vulkan_depth_4x_as_1x rewrites the surface info for one draw only - the
  // guest may keep drawing with it, and every draw decides for itself.
  struct SurfaceInfoRestore {
    VulkanCommandProcessor& command_processor;
    uint32_t surface_info;
    ~SurfaceInfoRestore() {
      command_processor.register_file_->values[XE_GPU_REG_RB_SURFACE_INFO] =
          surface_info;
      command_processor.render_target_cache_->SetDrawSamplesAsPixels(false);
    }
  } surface_info_restore{*this,
                         register_file_->values[XE_GPU_REG_RB_SURFACE_INFO]};

  // One-time confirmation of the effective guest depth path for this title
  // (after per-game config has loaded) - lets a setting like the per-game
  // vulkan_depth_unorm24 override be verified from the log.
  static std::atomic<bool> depth_path_logged{false};
  {
    bool expected = false;
    if (depth_path_logged.compare_exchange_strong(expected, true)) {
      bool unorm24 = render_target_cache_->depth_unorm24_vulkan_format_supported();
      XELOGI(
          "Depth path: vulkan_depth_unorm24(effective)={} -> guest 24-bit depth "
          "uses {}",
          cvars::vulkan_depth_unorm24, unorm24 ? "native D24_UNORM_S8" : "float32 emulation");
    }
  }

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  if (regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0) {
    // Doesn't actually draw. Matches the Direct3D 12 backend.
    // TODO(Triang3l): Do something so memexport still works in this case maybe?
    // Unlikely that zero would even really be legal though.
    return true;
  }

  const ui::vulkan::VulkanDevice::Properties& device_properties =
      GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  // TODO(Triang3l): If the shader uses memory export, but
  // vertexPipelineStoresAndAtomics is not supported, convert the vertex shader
  // to a compute shader and dispatch it after the draw if the draw doesn't use
  // tessellation.
  const bool memexport_used_vertex =
      vertex_shader->memexport_eM_written() != 0 &&
      device_properties.vertexPipelineStoresAndAtomics;
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done =
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader,
                                                             regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  const bool memexport_used_pixel = pixel_shader &&
                                    pixel_shader->memexport_eM_written() != 0 &&
                                    device_properties.fragmentStoresAndAtomics;
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  if (frame_dump_file_ && !issue_draw_memexport_retry_) {
    DumpFrameDraw(*vertex_shader, pixel_shader, prim_type, index_count,
                  index_buffer_info);
  }
  if (!issue_draw_memexport_retry_) {
    const uint32_t draw_index = frame_draw_index_++;
    if (draw_index >= skip_draws_first_ && draw_index <= skip_draws_last_ &&
        (draw_index < skip_draws_keep_first_ ||
         draw_index > skip_draws_keep_last_)) {
      return true;
    }
  }

  // Memory export read back from the host-mapped buffer (no guest memory
  // import to export into), for which the GPU snapshots the export ranges
  // before the draw. With memexport_readback_deferred, this export's snapshot
  // goes after those of the exports still pending; read back right after the
  // draw, it reuses the buffer from its start. Without room, the pending ones
  // are read back first, here, before the draw records anything (awaiting the
  // GPU is safe only between draws).
  bool memexport_deferred = false;
  if (cvars::memexport_enable && !memexport_ranges_.empty() &&
      !shared_memory_->is_zero_copy() && shared_memory_->IsHostMapped() &&
      shared_memory_host_and_edram_descriptor_set_ == VK_NULL_HANDLE) {
    memexport_deferred = cvars::memexport_readback_deferred;
    uint32_t snapshot_size = 0;
    for (const draw_util::MemExportRange& range : memexport_ranges_) {
      snapshot_size += range.size_bytes;
    }
    if (!pending_memexport_readbacks_.empty() &&
        (!memexport_deferred ||
         pending_memexport_snapshot_used_ + uint64_t(snapshot_size) >
             memexport_snapshot_size_)) {
      // Completing every submission reads all the pending ones back.
      if (!AwaitAllQueueOperationsCompletion() ||
          !pending_memexport_readbacks_.empty()) {
        XELOGE("Memexport readback: the pending exports were not read back");
        return false;
      }
    }
    // The buffer is replaced only with nothing pending in it. Deferred, with
    // room for many exports - Crysis 3 exports up to 1 MB a frame while
    // loading.
    if (pending_memexport_readbacks_.empty() &&
        !EnsureMemexportSnapshotBuffer(
            memexport_deferred ? std::max(snapshot_size, uint32_t(4) << 20)
                               : snapshot_size)) {
      return false;
    }
  }

  // A pixel shader that may kill pixels leaves covered pixels unwritten, and
  // only a pixel shader does the alpha test (skip_overwritten_transfers).
  render_target_cache_->SetDrawPixelShaderKills(pixel_shader &&
                                                pixel_shader->kills_pixels());
  render_target_cache_->SetDrawHasPixelShader(pixel_shader != nullptr);

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(
                          regs.Get<reg::SQ_PROGRAM_CNTL>(),
                          regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
                   : 0;

  // The surface info of a 4x MSAA draw, before anything reads it.
  const bool depth_4x_as_1x = RewriteMsaa4xSurfaceInfoForDraw(
      pixel_shader, ps_param_gen_pos != UINT32_MAX);
  if (XE_UNLIKELY(render_target_cache_->IsEdramTraceActive())) {
    // What decides whether the draw overwrites its targets or may run per
    // sample, for the line of the first draw of a binding.
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    render_target_cache_->SetEdramTraceDrawInfo(fmt::format(
        " | vs {:016X} ps {:016X}{}{}{}{}{}, alpha test {} func {} a2c {}, "
        "cull {}{}, poly mode {}, primitive {} x{}",
        vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
        pixel_shader && pixel_shader->uses_texture_fetch_instruction_results()
            ? " textures"
            : "",
        ps_param_gen_pos != UINT32_MAX ? " position" : "",
        pixel_shader && pixel_shader->kills_pixels() ? " kill" : "",
        pixel_shader && pixel_shader->writes_depth() ? " depth" : "",
        depth_4x_as_1x
            ? (render_target_cache_->draw_samples_as_pixels_keep_vertical()
                   ? ", samples as pixels at 2x"
                   : ", samples as pixels")
            : "",
        uint32_t(rb_colorcontrol.alpha_test_enable),
        uint32_t(rb_colorcontrol.alpha_func),
        uint32_t(rb_colorcontrol.alpha_to_mask_enable),
        pa_su_sc_mode_cntl.cull_front ? "F" : "",
        pa_su_sc_mode_cntl.cull_back ? "B" : "",
        uint32_t(pa_su_sc_mode_cntl.poly_mode),
        uint32_t(vgt_draw_initiator.prim_type),
        uint32_t(vgt_draw_initiator.num_indices)));
  }

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  bool use_interpreter = false;
  bool drop_until_ready = false;
  uint32_t normalized_color_mask;
  reg::RB_DEPTHCONTROL normalized_depth_control;
  draw_util::HostDepthPolygonOffset host_depth_polygon_offset;
  bool apply_host_depth_polygon_offset = false;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  // One readiness snapshot per draw, taken where the sampler collection reads
  // it. Re-reading bindings_ready() later would claim binding counts for a
  // stage whose sampler vector was collected as empty.
  bool stage_bindings_ready[2] = {false, false};
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return false;
    }

    // Process primitives.
    {
      PassEndReasonScope pass_end_reason_scope(
          *this, PassEndReason::kPrimitiveProcessor);
      if (!primitive_processor_->Process(primitive_processing_result)) {
        return false;
      }
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    // TODO(Triang3l): Geometry-type-specific vertex shader, vertex shader as
    // compute.
    // Skip unsupported host vertex shader types (but allow tessellation types
    // through - they will be handled in pipeline creation or rejected there if
    // not fully supported yet). Both AsTriangleStrip fallbacks are needed on
    // hosts without geometry shader support (MoltenVK / MoltenVK-like); the
    // SPIR-V translator and pipeline cache both handle them end-to-end.
    if (primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(
            primitive_processing_result.host_vertex_shader_type)) {
      return false;
    }

    normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

    // Compute which color render targets are used.
    normalized_color_mask =
        pixel_shader ? draw_util::GetNormalizedColorMask(
                           regs, pixel_shader->writes_color_targets())
                     : 0;
    apply_host_depth_polygon_offset =
        pixel_shader && !pixel_shader->writes_depth() &&
        render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        draw_util::GetHostDepthPolygonOffsetIfNeeded(
            regs, primitive_polygonal, normalized_depth_control,
            normalized_color_mask, host_depth_polygon_offset);

    // Shader modifications.
    vertex_shader_modification =
        pipeline_cache_->GetCurrentVertexShaderModification(
            *vertex_shader, primitive_processing_result.host_vertex_shader_type,
            interpolator_mask, ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification =
        pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                           *pixel_shader, interpolator_mask, ps_param_gen_pos,
                           normalized_depth_control, normalized_color_mask,
                           apply_host_depth_polygon_offset)
                     : SpirvShaderTranslator::Modification(0);

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(
            vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(
                               pixel_shader_modification.value))
                     : nullptr;
    // Decide whether the ucode interpreter can stand in for the real VS while
    // it translates+compiles in the background: a plain (non-tessellated,
    // non-expanded) vertex shader with static control flow and no texture fetch
    // or memory export, on a device matching the interpreter's assumptions
    // (single shared-memory binding, full 32-bit indices). When eligible, VS
    // translation is deferred to the background creation thread.
    use_interpreter =
        cvars::vulkan_placeholder_pipelines &&
        cvars::async_shader_vs_interpreter &&
        !vertex_shader_translation->is_translated() &&
        active_vertex_shader_ucode_address_ != 0 &&
        device_properties.fullDrawIndexUint32 &&
        SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
            device_properties.maxStorageBufferRange) == 0 &&
        primitive_processing_result.host_vertex_shader_type ==
            Shader::HostVertexShaderType::kVertex &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kPointList &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kRectangleList &&
        primitive_processing_result.host_primitive_type !=
            xenos::PrimitiveType::kQuadList &&
        vertex_shader->texture_bindings().empty() &&
        !vertex_shader->uses_subroutine_calls() &&
        vertex_shader->memexport_eM_written() == 0 &&
        vertex_shader->constant_register_map().loop_bitmap == 0;
    bool async_available =
        pipeline_cache_->CanCreatePipelineAsync(pixel_shader != nullptr);
    // A draw with no placeholder to render it now: the interpreter can't stand
    // in AND the real VS isn't translated yet (so no real-VS + no-op-PS
    // placeholder either). These are the draws async_shader_skip_draws governs.
    // Interpreter-eligible draws and draws whose VS is already translated
    // always have a placeholder and never translate on the draw thread.
    bool no_placeholder = cvars::vulkan_placeholder_pipelines &&
                          async_available && !use_interpreter &&
                          !vertex_shader_translation->is_translated();
    // The draw thread translates only for a no-placeholder draw that isn't
    // being skipped (so it can render via a real-VS placeholder).
    bool translate_here =
        !async_available || (no_placeholder && !cvars::async_shader_skip_draws);
    if (translate_here) {
      if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                    pixel_shader_translation)) {
        return false;
      }
    }
    drop_until_ready = cvars::vulkan_placeholder_pipelines && no_placeholder &&
                       cvars::async_shader_skip_draws;

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // Cross-draw cache (cvar vulkan_cache_sampler_parameters): a stage's
    // previous parameters and handles are reused when the binding list (the
    // shader) is the same and no sampler was destroyed since; parameters are
    // re-derived only for fetch slots written since the last draw, and
    // UseSampler (whose at-least-once-per-submission contract keeps handles
    // safe from the LRU eviction) is skipped when the stage's last full
    // UseSampler pass already happened in the current submission.
    uint32_t samplers_overflowed_count = 0;
    const bool sampler_cache_handles_valid =
        cvars::vulkan_cache_sampler_parameters &&
        current_samplers_destroy_generation_ ==
            texture_cache_->sampler_destroy_generation();
    bool sampler_stage_validated[2] = {false, false};
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>&
          shader_samplers =
              j ? current_samplers_pixel_ : current_samplers_vertex_;
      const VulkanShader*& cached_samplers_shader =
          j ? current_samplers_shader_pixel_ : current_samplers_shader_vertex_;
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader) {
        // Downstream consumers (texture set hash, descriptor writes) iterate
        // the whole vector - it must match this draw's (empty) binding list.
        shader_samplers.clear();
        cached_samplers_shader = nullptr;
        continue;
      }
      // Don't read a shader's sampler bindings while a creation thread is still
      // populating them (async draws don't translate here). A placeholder draw
      // that ends up using this shader binds no samplers for it anyway.
      if (!i) {
        stage_bindings_ready[j] = shader->bindings_ready();
      }
      if (!stage_bindings_ready[j]) {
        if (!i) {
          shader_samplers.clear();
        }
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i && sampler_cache_handles_valid &&
          cached_samplers_shader == shader &&
          shader_samplers.size() == shader_sampler_bindings.size()) {
        // Cache hit for this stage.
        const uint32_t fetch_up_to_date =
            j ? current_samplers_fetch_up_to_date_pixel_
              : current_samplers_fetch_up_to_date_vertex_;
        const bool submission_changed =
            (j ? current_samplers_submission_pixel_
               : current_samplers_submission_vertex_) !=
            GetCurrentSubmission();
        for (size_t k = 0; k < shader_sampler_bindings.size(); ++k) {
          std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
              shader_sampler_pair = shader_samplers[k];
          bool need_use_sampler =
              submission_changed ||
              shader_sampler_pair.second == VK_NULL_HANDLE;
          if (!(fetch_up_to_date &
                (uint32_t(1) << shader_sampler_bindings[k].fetch_constant))) {
            VulkanTextureCache::SamplerParameters parameters =
                texture_cache_->GetSamplerParameters(
                    shader_sampler_bindings[k]);
            if (parameters != shader_sampler_pair.first) {
              shader_sampler_pair.first = parameters;
              shader_sampler_pair.second = VK_NULL_HANDLE;
              need_use_sampler = true;
            }
          }
          if (need_use_sampler) {
            bool sampler_overflowed;
            VkSampler shader_sampler = texture_cache_->UseSampler(
                shader_sampler_pair.first, sampler_overflowed);
            shader_sampler_pair.second = shader_sampler;
            if (shader_sampler == VK_NULL_HANDLE) {
              if (!sampler_overflowed) {
                return false;
              }
              ++samplers_overflowed_count;
            }
          }
        }
        sampler_stage_validated[j] = true;
        continue;
      }
      // Full pass: cache disabled or missed, or the i == 1 overflow retry
      // (where the parameters derived at i == 0 are kept, but every sampler
      // must be revalidated with UseSampler in the possibly new submission).
      if (!i) {
        shader_samplers.clear();
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding :
             shader_sampler_bindings) {
          shader_samplers.emplace_back(
              texture_cache_->GetSamplerParameters(shader_sampler_binding),
              VK_NULL_HANDLE);
        }
      }
      // Record the binding-list identity before the UseSampler loop so a
      // mid-loop failure return can't leave the vector holding this shader's
      // parameters while the cached key still names the previous shader
      // (the stamps below are only committed on full success, so the next
      // draw re-derives dirty slots and UseSamplers null handles as needed).
      cached_samplers_shader = shader;
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
               shader_sampler_pair : shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler = texture_cache_->UseSampler(
            shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return false;
          }
          ++samplers_overflowed_count;
        }
      }
      sampler_stage_validated[j] = true;
    }
    if (!samplers_overflowed_count) {
      // The sampler pass completed with every needed UseSampler call made in
      // the current submission - commit the cache stamps. Only stages
      // actually validated this draw get their dirty masks cleared; a skipped
      // stage's cached parameters may still reference fetch slots that were
      // written and not re-derived.
      if (cvars::vulkan_cache_sampler_parameters) {
        const uint64_t submission_now = GetCurrentSubmission();
        if (sampler_stage_validated[0]) {
          current_samplers_fetch_up_to_date_vertex_ = ~uint32_t(0);
          current_samplers_submission_vertex_ = submission_now;
        }
        if (sampler_stage_validated[1]) {
          current_samplers_fetch_up_to_date_pixel_ = ~uint32_t(0);
          current_samplers_submission_pixel_ = submission_now;
        }
        current_samplers_destroy_generation_ =
            texture_cache_->sampler_destroy_generation();
      }
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(
            samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionCompletionAndDeviceLoss(sampler_overflow_await_submission);
  }

  // Set up the render targets - this may perform dispatches and draws.
  {
    PassEndReasonScope pass_end_reason_scope(*this,
                                             PassEndReason::kRenderTargets);
    if (!render_target_cache_->Update(is_rasterization_done,
                                      normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      return false;
    }
  }

  // merge_tiling_bands: the multisampled render targets the first band draws
  // into are drawn as tall as the screen - a scissor covering the band covers
  // the screen, and the draws first executed in a later band are placed at
  // its rows. They are recorded as the first band draws into them (each
  // starting the later bands' rows with what the first band starts with).
  tiling_band_draw_y_offset_ = 0;
  tiling_band_draw_expand_scissor_ = false;
  tiling_band_replicate_ = 0;
  if (tiling_band_merge_active()) {
    if (tiling_band_sequence_seen_ != tiling_band_sequence()) {
      tiling_band_sequence_seen_ = tiling_band_sequence();
      render_target_cache_->ResetTilingBandRenderTargets();
      // The rows of a band - the first band's window scissor.
      auto window_scissor_tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      auto window_scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      tiling_band_rows_ =
          window_scissor_br.br_y > window_scissor_tl.tl_y
              ? window_scissor_br.br_y - window_scissor_tl.tl_y
              : 0;
      if (tiling_band() != 0 || window_scissor_tl.tl_y != 0 ||
          !tiling_band_rows_ || (tiling_band_rows_ & 7) ||
          tiling_band_rows_ * 2 > RenderTargetCache::kMergedTilingBandsHeight ||
          render_target_cache_->GetDrawScaleX() != 1 ||
          render_target_cache_->GetDrawScaleY() != 1) {
        DisableTilingBandMerge();
      }
    }
    bool into_band_targets = false;
    if (tiling_band_merge_active()) {
      tiling_band_replicate_ = render_target_cache_->NoteTilingBandDraw(
          tiling_band() == 0, normalized_depth_control, normalized_color_mask,
          *vertex_shader, into_band_targets);
    }
    if (into_band_targets) {
      tiling_band_draw_expand_scissor_ = true;
      if (tiling_band_draw_from_later_band()) {
        tiling_band_draw_y_offset_ =
            int32_t(uint32_t(tiling_band()) * tiling_band_rows_);
      }
    }
  }

  // The signs of the textures of the first fetch constants the pixel shader
  // fetches, for the specialization constants of its pipeline
  // (spirv_texture_sign_specialization) - from the fetch constants, like the
  // swizzled signs the texture cache gives the shaders. Where it binds no
  // texture (taking the signs as unsigned), the classes made known (unsigned
  // or gamma) give the same zeros.
  uint32_t texture_sign_classes = 0;
  if (pixel_shader && cvars::spirv_texture_sign_specialization &&
      cvars::vulkan_texture_sign_classes) {
    // The slots the translator gave the fetch constants.
    std::array<uint8_t, 32> texture_sign_class_slots;
    SpirvShaderTranslator::GetTextureSignClassSlots(
        *pixel_shader, cvars::spirv_texture_sign_specialization_used,
        texture_sign_class_slots);
    for (const Shader::TextureBinding& texture_binding :
         pixel_shader->texture_bindings()) {
      const uint32_t fetch_constant = texture_binding.fetch_constant;
      const uint32_t slot = texture_sign_class_slots[fetch_constant];
      if (slot >= SpirvShaderTranslator::kTextureSignClassFetchConstantCount) {
        continue;
      }
      texture_sign_classes |=
          SpirvShaderTranslator::GetTextureSignClass(texture_util::SwizzleSigns(
              regs.GetTextureFetch(fetch_constant)))
          << (2 * slot);
    }
  }
  pipeline_cache_->SetTextureSignClasses(texture_sign_classes);
  // Whether every texture of the pixel shader has a zero exponent adjustment
  // (spirv_texture_exp_adjust_specialization; word 3 bits 13:18).
  bool texture_exp_adjust_zero = false;
  if (pixel_shader && cvars::spirv_texture_exp_adjust_specialization) {
    texture_exp_adjust_zero = true;
    for (const Shader::TextureBinding& texture_binding :
         pixel_shader->texture_bindings()) {
      if (regs.GetTextureFetch(texture_binding.fetch_constant).exp_adjust) {
        texture_exp_adjust_zero = false;
        break;
      }
    }
  }
  pipeline_cache_->SetTextureExpAdjustZero(texture_exp_adjust_zero);

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VulkanPipelineCache::Pipeline* pipeline;
  if (!pipeline_cache_->ConfigurePipeline(
          vertex_shader_translation, pixel_shader_translation,
          primitive_processing_result, normalized_depth_control,
          normalized_color_mask,
          render_target_cache_->last_update_render_pass_key(), use_interpreter,
          &pipeline)) {
    XELOGE("IssueDraw: ConfigurePipeline failed for VS={:016X} PS={:016X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return false;
  }

  // A draw exporting memory writes data later draws and the CPU use, not only
  // pixels. Recorded while its pipeline is still being created (or its shaders
  // translated), it would be dropped at replay and its output never written -
  // Crysis 3 generates index buffers that way while loading and when objects
  // break, and a dropped one keeps another mesh's indices (long spikes across
  // the screen). Wait for the creation and issue it again, once.
  if (!memexport_ranges_.empty() && !issue_draw_memexport_retry_ &&
      (pipeline->pipeline.load(std::memory_order_acquire) == VK_NULL_HANDLE ||
       !stage_bindings_ready[0] ||
       (pixel_shader && !stage_bindings_ready[1]))) {
    pipeline_cache_->AwaitPipelineCompletion();
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
      XELOGI(
          "IssueDraw: waited for the pipeline of a memory-exporting draw "
          "(VS {:016X}, PS {:016X}) instead of dropping it",
          vertex_shader->ucode_data_hash(),
          pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    }
    // From the state the guest set: vulkan_depth_4x_as_1x may have rewritten
    // the surface for this draw already, and the draw would then neither see
    // a 4x surface nor scale itself to the samples.
    register_file_->values[XE_GPU_REG_RB_SURFACE_INFO] =
        surface_info_restore.surface_info;
    render_target_cache_->SetDrawSamplesAsPixels(false);
    issue_draw_memexport_retry_ = true;
    const bool issued = IssueDraw(prim_type, index_count, index_buffer_info,
                                  major_mode_explicit);
    issue_draw_memexport_retry_ = false;
    return issued;
  }

  if (drop_until_ready) {
    // Non-interpretable draw whose shaders weren't translated yet - it has been
    // queued for background creation; skip drawing it until the real pipeline
    // is ready (a later frame renders it). Never translate/compile on the draw
    // thread for these.
    XELOGI(
        "Draw skipped (no interpreter placeholder, real pipeline not ready): "
        "VS {:016X}, PS {:016X}",
        vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return true;
  }
  // If async mode is active, this may be a placeholder pipeline. The real
  // pipeline will be swapped in by the creation thread when ready.
  VkPipeline current_pipeline =
      pipeline->pipeline.load(std::memory_order_acquire);
  if (cvars::vulkan_placeholder_pipelines &&
      current_pipeline == VK_NULL_HANDLE) {
    // Placeholder mode: real pipeline not created yet and no placeholder -
    // skip this draw rather than stalling the draw thread.
    XELOGI("Draw skipped (pipeline not ready yet): VS {:016X}, PS {:016X}",
           vertex_shader->ucode_data_hash(),
           pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    return true;
  }
  // Deferred-bind mode (cvar off): current_pipeline may legitimately be
  // VK_NULL_HANDLE here - fall through and record a deferred bind of the
  // stable slot pointer; EndSubmission waits (or replay drops it under
  // vulkan_async_skip_draws). pipeline_layout is never null (the slot was
  // created with at least the minimal layout).
  // The interpreter reads the guest ucode from shared memory by its program
  // address. A cached interpreter placeholder reused for an inline
  // (IM_LOAD_IMMEDIATE, address 0) shader can't be fed, so skip until the real
  // pipeline is ready rather than interpret from address 0.
  if (active_vertex_shader_ucode_address_ == 0 &&
      current_pipeline ==
          pipeline->placeholder_pipeline.load(std::memory_order_acquire) &&
      pipeline->uses_interpreter.load(std::memory_order_acquire)) {
    return true;
  }

  // Push debug marker with Xbox 360 draw context for RenderDoc annotation.
  // Done early so texture loads appear nested under the draw that uses them.
  if (debug_markers_enabled_) {
    char label[draw_util::kDebugMarkerLabelMaxLength];
    draw_util::FormatDrawDebugMarker(
        label, sizeof(label), prim_type, primitive_processing_result,
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0);
    PushDebugMarker("%s", label);
  }

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  // Only read after-translation state for stages ready at the draw's snapshot.
  uint32_t used_texture_mask =
      (stage_bindings_ready[0]
           ? vertex_shader->GetUsedTextureMaskAfterTranslation()
           : 0) |
      (pixel_shader != nullptr && stage_bindings_ready[1]
           ? pixel_shader->GetUsedTextureMaskAfterTranslation()
           : 0);
  {
    PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kTextures);
    texture_cache_->RequestTextures(used_texture_mask);
  }

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  // With asynchronous creation the handle may still be VK_NULL_HANDLE here -
  // a deferred bind of the stable slot pointer is recorded instead of
  // stalling. The pipeline cache's EndSubmission at the submission boundary
  // either waits for creation to complete (default) or lets the replay drop
  // the draws of still-unready pipelines (vulkan_async_skip_draws). The
  // de-dup compares slot POINTERS, not handle values.
  // Wrapped in a lambda so the same (EDS-aware) bind and descriptor-set
  // invalidation can be re-run after in-pass transfers replace the bound
  // pipeline (see below, after entering the render pass).
  auto bind_guest_graphics_pipeline = [&]() {
  // Bind the handle observed when this draw's bindings were decided
  // (current_pipeline, loaded above), not the slot at replay time: the
  // recorded descriptor sets cover only that pipeline's layout. A draw
  // recorded while the slot was empty is dropped at replay even if the
  // async creation finishes in between. De-dup on the handle, so the first
  // draw after the slot fills (or swaps placeholder to real) re-records.
  if (current_guest_graphics_pipeline_ != &pipeline->pipeline ||
      current_guest_graphics_pipeline_handle_ != current_pipeline) {
    deferred_command_buffer_.CmdVkBindPipelineDeferred(
        VK_PIPELINE_BIND_POINT_GRAPHICS, current_pipeline);
    current_guest_graphics_pipeline_ = &pipeline->pipeline;
    current_guest_graphics_pipeline_handle_ = current_pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }
  auto pipeline_layout = static_cast<const PipelineLayout*>(
      pipeline->pipeline_layout.load(std::memory_order_acquire));
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible). The two texture sets
      // are the highest enum indices, so a change of the vertex texture set
      // layout breaks compatibility for the pixel texture set too; a pixel-only
      // change breaks only the pixel set.
      //
      // Previously the unconditional clear of the texture values-up-to-date
      // bits in UpdateBindings made this case safe even though the computed
      // descriptor_sets_kept was discarded. Now that the value cache may keep
      // those bits set, this block must actively invalidate the values bit,
      // the bound bit, and the value-cache hash for every texture set whose
      // layout is no longer compatible - otherwise a set written under layout A
      // could be skipped and bound under an incompatible layout B.
      uint32_t descriptor_sets_kept =
          uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_
              ->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept,
            uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
      // Invalidate every texture set at or above the lowest changed index
      // (the cascade: incompatibility for set N breaks all sets >= N). This
      // supersedes upstream's `bound_up_to_date_ &= (1<<descriptor_sets_kept)-1`
      // masking: in this block descriptor_sets_kept never drops below
      // kDescriptorSetTexturesVertex, so the per-set clears below cover the same
      // bound bits AND add the value/hash invalidation the texture-set cache needs.
      if (descriptor_sets_kept <=
          uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
        current_graphics_descriptor_sets_bound_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
        current_texture_descriptor_set_hash_valid_vertex_ = false;
      }
      if (descriptor_sets_kept <=
          uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
        current_graphics_descriptor_sets_bound_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
        current_texture_descriptor_set_hash_valid_pixel_ = false;
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state. The transient pool has NOT reset within the frame
      // (that path resets the hash validity at frame open), so the cached
      // texture sets are still valid memory; clearing the bound bits forces a
      // re-bind of the still-valid sets without a re-write. Do not invalidate
      // the hash here - the value cache stays usable across an intra-frame
      // submission/command-buffer restart.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }
  };
  bind_guest_graphics_pipeline();

  bool host_render_targets_used = render_target_cache_->GetPath() ==
                                  RenderTargetCache::Path::kHostRenderTargets;

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;

  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.

  // Resolution scale of this draw, which may be 1x1 because of
  // draw_resolution_scale_threshold, which the divisors have to match too.
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();
  // ZPD segments can't mix scales. The resolved sample count is divided by
  // one scale area per segment. Split before the FSI counter index goes
  // into system constants.
  // A 4x depth draw into the 1x surface of its samples covers as many host
  // pixels per guest pixel as a 4x one has samples. A 4x surface stored at 2x
  // (msaa_4x_as_2x) passes half the guest's samples.
  {
    const xenos::MsaaSamples guest_msaa_samples =
        regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
    // Into a 2x surface twice as wide: 2 host pixels of 2 samples.
    UpdateZPDScale(
        ((draw_resolution_scale_x * draw_resolution_scale_y) >>
         (depth_4x_as_1x
              ? (render_target_cache_->draw_samples_as_pixels_keep_vertical()
                     ? 1
                     : 2)
              : 0)) |
        (render_target_cache_->GetHostMsaaSamples(guest_msaa_samples) !=
                 guest_msaa_samples
             ? kZPDScaleHalfSamples
             : 0));
  }
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(
      draw_resolution_scale_x, draw_resolution_scale_y,
      draw_resolution_scale_x > 1
          ? (depth_4x_as_1x ? divisors::MagicDiv(draw_resolution_scale_x)
                            : texture_cache_->draw_resolution_scale_x_divisor())
          : divisors::MagicDiv(1),
      draw_resolution_scale_y > 1
          ? (depth_4x_as_1x ? divisors::MagicDiv(draw_resolution_scale_y)
                            : texture_cache_->draw_resolution_scale_y_divisor())
          : divisors::MagicDiv(1),
      false, device_properties.maxViewportDimensions[0],
      device_properties.maxViewportDimensions[1],
      cvars::vulkan_allow_reverse_z, normalized_depth_control,
      host_render_targets_used &&
          render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth());
  gviargs.SetupRegisterValues(regs);

  draw_util::GetHostViewportInfo(&gviargs, viewport_info);
  // vulkan_shading_rate: coarse shading of the multisampled scene's draws,
  // where the rate is allowed with what the pixel shader writes.
  draw_shading_rate_ = 0;
  if (cvars::vulkan_shading_rate && pixel_shader &&
      device_properties.pipelineFragmentShadingRate) {
    const xenos::MsaaSamples guest_msaa_samples =
        regs.Get<reg::RB_SURFACE_INFO>().msaa_samples;
    if (guest_msaa_samples != xenos::MsaaSamples::k1X &&
        (uint32_t(1) << uint32_t(render_target_cache_->GetHostMsaaSamples(
             guest_msaa_samples))) <=
            uint32_t(
                device_properties.maxFragmentShadingRateRasterizationSamples) &&
        (!pixel_shader->writes_depth() ||
         device_properties.fragmentShadingRateWithShaderDepthStencilWrites) &&
        // Alpha to coverage is the shader's sample mask unless done by the
        // host's fixed function (color output 0 needed).
        (!regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable ||
         device_properties.fragmentShadingRateWithShaderSampleMask ||
         (cvars::host_alpha_to_coverage && (normalized_color_mask & 0xF)))) {
      draw_shading_rate_ = uint32_t(cvars::vulkan_shading_rate) & 3;
    }
  }
  // Update dynamic graphics pipeline state.
  UpdateDynamicState(viewport_info, primitive_polygonal,
                     normalized_depth_control, draw_resolution_scale_x,
                     draw_resolution_scale_y, apply_host_depth_polygon_offset,
                     pipeline->dynamic_state);

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kVertex;

  // Update system constants before uploading them.
  UpdateSystemConstantValues(
      primitive_polygonal, primitive_processing_result, shader_32bit_index_dma,
      viewport_info, used_texture_mask, normalized_depth_control,
      normalized_color_mask,
      apply_host_depth_polygon_offset ? &host_depth_polygon_offset : nullptr);

  // Whether we bound the placeholder pipeline (vs the real one). Derived from
  // the handle actually bound, so it's consistent with current_pipeline even if
  // the creation thread swaps in the real pipeline mid-draw (the is_placeholder
  // flag is cleared a few instructions later, so reading it separately can
  // disagree).
  bool bound_is_placeholder =
      current_pipeline ==
      pipeline->placeholder_pipeline.load(std::memory_order_acquire);
  // The interpreter placeholder (interpreter VS + no-op PS) also needs its
  // ucode location + full float constants fed to it.
  bool interpreter_placeholder =
      bound_is_placeholder &&
      pipeline->uses_interpreter.load(std::memory_order_acquire);
  // Any placeholder draw binds the no-op placeholder pixel shader (which never
  // samples), so its pixel textures/samplers must not be bound - the
  // placeholder pipeline's layout has none.
  bool placeholder_pixel_shader = bound_is_placeholder;
  {
    uint32_t ucode_base_dwords = 0, cf_instr_count = 0;
    if (interpreter_placeholder) {
      uint32_t ucode_address = active_vertex_shader_ucode_address_;
      ucode_base_dwords = ucode_address >> 2;
      cf_instr_count = vertex_shader->cf_pair_index_bound() * 2;
      shared_memory_->RequestRange(
          ucode_address,
          uint32_t(vertex_shader->ucode_dword_count()) * sizeof(uint32_t));
    }
    if (system_constants_.interpreter_ucode_base_dwords != ucode_base_dwords ||
        system_constants_.interpreter_cf_instr_count != cf_instr_count) {
      system_constants_.interpreter_ucode_base_dwords = ucode_base_dwords;
      system_constants_.interpreter_cf_instr_count = cf_instr_count;
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
    }
  }

  // Two-buffer memexport routing: producer and geometry-consumer draws use the
  // host-imported buffer (aliasing guest RAM) so memexport output stays CPU
  // coherent (fixing the page false-sharing clobber) and consumers read it
  // directly. Only texture-sampled ranges are copied into the device buffer
  // (EnsureMemexportRangeInDeviceBuffer). Inert without the host buffer.
  bool route_to_host = false;
  if (shared_memory_host_and_edram_descriptor_set_ != VK_NULL_HANDLE) {
    route_to_host =
        memexport_used_vertex || memexport_used_pixel ||
        (any_memexport_pages_written_ &&
         ((primitive_processing_result.index_buffer_type ==
               PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
           IsMemexportRange(
               primitive_processing_result.guest_index_base,
               primitive_processing_result.guest_draw_vertex_count *
                   uint32_t(sizeof(uint32_t)))) ||
          VertexFetchInMemexportRange(regs, *vertex_shader)));
  }
  {
    // Select this draw's shared-memory set, forcing a re-bind only if it
    // changed. Both sets are pre-written, so the values-up-to-date bits stay
    // set (never cleared here - the flush asserts on them); only the bound-set
    // tracking is touched.
    VkDescriptorSet shared_memory_set =
        route_to_host ? shared_memory_host_and_edram_descriptor_set_
                      : shared_memory_and_edram_descriptor_set_;
    if (current_graphics_descriptor_sets_
            [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] !=
        shared_memory_set) {
      current_graphics_descriptor_sets_
          [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
              shared_memory_set;
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          ~(UINT32_C(1)
            << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram);
    }
  }

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  {
    PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kBindings);
    if (!UpdateBindings(vertex_shader, pixel_shader, stage_bindings_ready[0],
                        stage_bindings_ready[1], interpreter_placeholder,
                        placeholder_pixel_shader)) {
      return false;
    }
  }

  // Ensure vertex buffers are resident.
  //
  // Use the vertex_fetch_bitmap instead of vertex_bindings() to avoid using
  // cached/stale vertex binding indices. The bitmap is populated during shader
  // translation and represents which fetch constant indices the shader actually
  // references, allowing us to check the current register values at draw time.
  const Shader::ConstantRegisterMap& constant_map_vertex =
      vertex_shader->constant_register_map();
  {
    uint32_t vfetch_addresses[96];
    uint32_t vfetch_sizes[96];
    uint32_t vfetch_current_queued = 0;
    for (uint32_t i = 0;
         i < xe::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
      uint32_t vfetch_bits_remaining =
          constant_map_vertex.vertex_fetch_bitmap[i];
      uint32_t j;
      while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
        vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
        uint32_t vfetch_index = i * 32 + j;
        xenos::xe_gpu_vertex_fetch_t vfetch_constant =
            regs.GetVertexFetch(vfetch_index);
        switch (vfetch_constant.type) {
          case xenos::FetchConstantType::kVertex:
            break;
          case xenos::FetchConstantType::kInvalidVertex:
            if (cvars::gpu_allow_invalid_fetch_constants) {
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" "
                "type! "
                "This is incorrect behavior, but you can try bypassing this by "
                "launching Xenia with "
                "--gpu_allow_invalid_fetch_constants=true.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
            return false;
          default:
            // Type is kTexture (2) or kInvalidTexture (3) - completely wrong
            // for vertex data
            if (cvars::gpu_allow_invalid_fetch_constants) {
              XELOGW(
                  "Vertex fetch constant {} ({:08X} {:08X}) has wrong type {} "
                  "(texture fetch constant in vertex slot) - allowing due to "
                  "--gpu_allow_invalid_fetch_constants=true. This will likely "
                  "crash or produce garbage!",
                  vfetch_index, vfetch_constant.dword_0,
                  vfetch_constant.dword_1,
                  static_cast<uint32_t>(vfetch_constant.type));
              break;
            }
            XELOGW(
                "Vertex fetch constant {} ({:08X} {:08X}) is completely "
                "invalid! Type={} - this slot contains a texture fetch "
                "constant (type 2), not a vertex fetch constant (type 0). "
                "This may indicate the shader is reading from the wrong fetch "
                "constant index, or the game has a bug.",
                vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1,
                static_cast<uint32_t>(vfetch_constant.type));
            return false;
        }
        vfetch_addresses[vfetch_current_queued] = vfetch_constant.address;
        vfetch_sizes[vfetch_current_queued++] = vfetch_constant.size;
      }
    }
    // The pixel shader may fetch vertices too
    // (vulkan_pixel_shader_vertex_buffers): its own fetch constants follow the
    // vertex shader's. Draws were never dropped over them, so an invalid one
    // or a failed request is skipped.
    const uint32_t vfetch_vertex_queued = vfetch_current_queued;
    if (pixel_shader && cvars::vulkan_pixel_shader_vertex_buffers) {
      const Shader::ConstantRegisterMap& constant_map_pixel =
          pixel_shader->constant_register_map();
      for (uint32_t i = 0;
           i < xe::countof(constant_map_pixel.vertex_fetch_bitmap); ++i) {
        uint32_t vfetch_bits_remaining =
            constant_map_pixel.vertex_fetch_bitmap[i] &
            ~constant_map_vertex.vertex_fetch_bitmap[i];
        uint32_t j;
        while (xe::bit_scan_forward(vfetch_bits_remaining, &j)) {
          vfetch_bits_remaining = xe::clear_lowest_bit(vfetch_bits_remaining);
          xenos::xe_gpu_vertex_fetch_t vfetch_constant =
              regs.GetVertexFetch(i * 32 + j);
          if (vfetch_constant.type == xenos::FetchConstantType::kVertex) {
            vfetch_addresses[vfetch_current_queued] = vfetch_constant.address;
            vfetch_sizes[vfetch_current_queued++] = vfetch_constant.size;
          }
        }
      }
    }

    if (vfetch_current_queued) {
      PassEndReasonScope pass_end_reason_scope(*this,
                                               PassEndReason::kSharedMemory);
      // Pre-acquire the critical region so we're not repeatedly re-acquiring
      // it in RequestRange - SharedMemory tracks dirty pages and only uploads
      // what actually changed, making redundant calls cheap under a hoisted
      // lock.
      auto shared_memory_request_range_hoisted =
          global_critical_region::Acquire();

      for (uint32_t i = 0; i < vfetch_current_queued; ++i) {
        if (!shared_memory_->RequestRange(vfetch_addresses[i] << 2,
                                          vfetch_sizes[i] << 2) &&
            i < vfetch_vertex_queued) {
          XELOGE(
              "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
              "shared memory",
              vfetch_addresses[i] << 2, vfetch_sizes[i] << 2);
          return false;
        }
      }
    }
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords
                                          << 2;
    // Host-routed producers write output to host_buffer_ (guest RAM), not the
    // device buffer, so this upload is redundant. It also drops the draw when
    // the guest committed only part of the declared capacity, so skip it.
    if (!route_to_host &&
        !shared_memory_->RequestRange(memexport_range_base_bytes,
                                      memexport_range.size_bytes)) {
      XELOGE(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start =
        std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end,
                 memexport_range_base_bytes + memexport_range.size_bytes);
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  {
    PassEndReasonScope pass_end_reason_scope(*this,
                                             PassEndReason::kSharedMemory);
    if (memexport_extent_start < memexport_extent_end) {
      shared_memory_->Use(
          VulkanSharedMemory::Usage::kGuestDrawReadWrite,
          std::make_pair(memexport_extent_start,
                         memexport_extent_end - memexport_extent_start));
    } else {
      // With in-pass resolves, fragment shaders may write shared memory inside
      // any guest pass - declare the write usage up front so no barrier is
      // needed at the resolve point.
      shared_memory_->Use(
          (render_target_cache_->local_read_attachments() &&
           !cvars::vulkan_in_pass_resolve_debug_read_usage)
              ? VulkanSharedMemory::Usage::kGuestDrawReadWrite
              : VulkanSharedMemory::Usage::kRead);
    }
  }

  // The memexport readback fallback (after the draw) needs the export ranges as
  // the draw finds them: the GPU copies them into memexport_snapshot_buffer_,
  // after the uploads above and before the draw writes.
  const bool memexport_readback = !route_to_host && cvars::memexport_enable &&
                                  !memexport_ranges_.empty() &&
                                  !shared_memory_->is_zero_copy();
  if (memexport_readback) {
    if (!shared_memory_->IsHostMapped()) {
      XELOGE("Memexport readback requires a host-mapped shared memory buffer "
             "when guest RAM cannot be imported (readback_resolve=uma)");
      return false;
    }
    memexport_snapshot_regions_.clear();
    // Deferred, after the snapshots of the exports still pending (room was
    // made at the start of the draw).
    const VkDeviceSize snapshot_start =
        memexport_deferred ? pending_memexport_snapshot_used_ : 0;
    VkDeviceSize snapshot_offset = snapshot_start;
    for (const draw_util::MemExportRange& range : memexport_ranges_) {
      VkBufferCopy& region = memexport_snapshot_regions_.emplace_back();
      region.srcOffset = VkDeviceSize(range.base_address_dwords) << 2;
      region.dstOffset = snapshot_offset;
      region.size = range.size_bytes;
      snapshot_offset += range.size_bytes;
    }
    if (snapshot_offset > memexport_snapshot_size_) {
      XELOGE("Memexport readback: no room for a {} KB snapshot",
             (snapshot_offset - snapshot_start) >> 10);
      return false;
    }
    if (memexport_deferred) {
      for (size_t i = 0; i < memexport_ranges_.size(); ++i) {
        pending_memexport_readbacks_.push_back(PendingMemexportReadback{
            GetCurrentSubmission(),
            memexport_ranges_[i].base_address_dwords << 2,
            memexport_ranges_[i].size_bytes,
            uint32_t(memexport_snapshot_regions_[i].dstOffset)});
      }
      pending_memexport_snapshot_used_ = uint32_t(snapshot_offset);
      if (memexport_log_left_) {
        --memexport_log_left_;
        std::string export_ranges;
        for (const draw_util::MemExportRange& range : memexport_ranges_) {
          export_ranges += fmt::format(" {:08X}+{:X}",
                                       range.base_address_dwords << 2,
                                       range.size_bytes);
        }
        XELOGI(
            "MemexportLog: export at draw {} of frame {}, submission {}, {} "
            "bytes, {} pending:{}",
            frame_draw_index_ - 1, frame_current_, GetCurrentSubmission(),
            uint32_t(snapshot_offset - snapshot_start),
            pending_memexport_readbacks_.size(), export_ranges);
      }
      // A fence or coherency request the guest observes awaits the GPU first
      // (command_processor_memexport.inc), which reads these back.
      memexport_await_pending_ = true;
      memexport_readbacks_pending_ = true;
      memexport_readbacks_in_submission_ = true;
    }
    PushBufferMemoryBarrier(
        shared_memory_->buffer(), memexport_extent_start,
        memexport_extent_end - memexport_extent_start,
        guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED, false);
    SubmitBarriers(true);
    deferred_command_buffer_.CmdVkCopyBuffer(
        shared_memory_->buffer(), memexport_snapshot_buffer_,
        uint32_t(memexport_snapshot_regions_.size()),
        memexport_snapshot_regions_.data());
    // The draw writes after the copy has read, and the host reads the copy
    // once the draw's submission completes.
    PushBufferMemoryBarrier(
        shared_memory_->buffer(), memexport_extent_start,
        memexport_extent_end - memexport_extent_start,
        VK_PIPELINE_STAGE_TRANSFER_BIT, guest_shader_pipeline_stages_,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    PushBufferMemoryBarrier(memexport_snapshot_buffer_, snapshot_start,
                            snapshot_offset - snapshot_start,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
                            VK_QUEUE_FAMILY_IGNORED, false);
  }

  // vulkan_debug_extra_pass_breaks: break the guest pass before every Nth draw
  // to measure what a break costs.
  if (cvars::vulkan_debug_extra_pass_breaks && in_render_pass_ &&
      ++debug_extra_pass_break_draws_ >=
          uint32_t(std::abs(cvars::vulkan_debug_extra_pass_breaks))) {
    debug_extra_pass_break_draws_ = 0;
    if (cvars::vulkan_debug_extra_pass_breaks < 0) {
      PushBufferMemoryBarrier(
          shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
          guest_shader_pipeline_stages_, guest_shader_pipeline_stages_,
          VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
      SubmitBarriers(true);
    } else {
      EndRenderPass();
    }
  }

  // After all commands that may dispatch, copy or insert barriers, submit the
  // barriers (may end the render pass), and (re)enter the render pass before
  // drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());

  // Encode render-target transfers that the render target cache queued for
  // execution inside this draw's render pass (avoids breaking the pass on
  // tile-based GPUs). If they can't be merged into the active pass, fall back
  // to performing them in a separate pass and re-enter the guest pass. Either
  // way the transfers change pipeline / dynamic / binding state, so re-emit it
  // before the actual guest draw below.
  if (render_target_cache_->HasPendingDrawPassTransfers()) {
    // An open native occlusion query of the guest would count the samples of
    // the transfer draws too (FSI counter queries only count guest shaders) -
    // close its segment around them, like a render pass end does.
    const bool suspend_zpd_segment =
        zpd_active_segment_.segment_active && !zpd_active_query_is_fsi_;
    const uint32_t zpd_scale_area = zpd_active_segment_.scale_area;
    if (suspend_zpd_segment) {
      CloseQuerySegment();
      if (cvars::log_gpu_frame_time_breakdown) {
        ++vk_frame_sync_stats_.zpd_transfer_suspends;
      }
    }
    if (!render_target_cache_->EncodePendingDrawPassTransfers()) {
      if (!render_target_cache_->FlushPendingDrawPassTransfers()) {
        return false;
      }
      SubmitBarriersAndEnterRenderTargetCacheRenderPass(
          render_target_cache_->last_update_render_pass(),
          render_target_cache_->last_update_framebuffer());
    }
    if (suspend_zpd_segment) {
      OpenQuerySegment(false);
      UpdateZPDScale(zpd_scale_area);
    }
    // Re-bind the guest pipeline (deferred, EDS-aware, with descriptor-set
    // invalidation) - the transfer draws bound their own external pipelines and
    // cleared the guest pipeline/layout state.
    bind_guest_graphics_pipeline();
    UpdateDynamicState(viewport_info, primitive_polygonal,
                       normalized_depth_control, draw_resolution_scale_x,
                       draw_resolution_scale_y, apply_host_depth_polygon_offset,
                       pipeline->dynamic_state);
    if (!UpdateBindings(vertex_shader, pixel_shader, stage_bindings_ready[0],
                        stage_bindings_ready[1], interpreter_placeholder,
                        placeholder_pixel_shader)) {
      return false;
    }
  }
  // Transfers of what a resolve clear left in their source - clears of the
  // destinations in this pass, after the transfers (no state of the draw is
  // changed by them).
  render_target_cache_->EncodePendingDrawPassClears();

  // merge_tiling_bands: the rows of the later bands start like the first
  // band's - copied outside the render pass, then the draw's state again.
  if (tiling_band_replicate_) {
    render_target_cache_->ReplicateTilingBandRows(tiling_band_replicate_,
                                                  tiling_band_rows_);
    tiling_band_replicate_ = 0;
    SubmitBarriersAndEnterRenderTargetCacheRenderPass(
        render_target_cache_->last_update_render_pass(),
        render_target_cache_->last_update_framebuffer());
    bind_guest_graphics_pipeline();
    UpdateDynamicState(viewport_info, primitive_polygonal,
                       normalized_depth_control, draw_resolution_scale_x,
                       draw_resolution_scale_y, apply_host_depth_polygon_offset,
                       pipeline->dynamic_state);
    if (!UpdateBindings(vertex_shader, pixel_shader, stage_bindings_ready[0],
                        stage_bindings_ready[1], interpreter_placeholder,
                        placeholder_pixel_shader)) {
      return false;
    }
  }

  // Track for device-lost diagnostics.
  ++submission_in_progress_.draw_count;
  submission_in_progress_.last_vs_hash = vertex_shader->ucode_data_hash();
  submission_in_progress_.last_ps_hash =
      pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  // pm4_bin_trace frames: draw usage per shader pair and pass size (PipeUse).
  if (XE_UNLIKELY(bin_trace_.frames_left) && current_framebuffer_) {
    PipelineUse& use = pipeline_use_[std::make_tuple(
        submission_in_progress_.last_vs_hash,
        submission_in_progress_.last_ps_hash,
        (uint32_t(current_framebuffer_->host_extent.width) << 16) |
            uint32_t(current_framebuffer_->host_extent.height))];
    ++use.draws;
    use.vertices += primitive_processing_result.host_draw_vertex_count;
    uint32_t textures_remaining =
        pixel_shader && stage_bindings_ready[1]
            ? pixel_shader->GetUsedTextureMaskAfterTranslation()
            : 0;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t sign_class = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        sign_class = std::max(sign_class, (signs >> (i * 2)) & 3);
      }
      ++use.texture_signs[sign_class];
    }
    if (pixel_shader) {
      auto color_control = regs.Get<reg::RB_COLORCONTROL>();
      uint32_t alpha_mode = 3;
      if (pixel_shader_modification.pixel.depth_stencil_mode ==
          SpirvShaderTranslator::Modification::DepthStencilMode::
              kNoAlphaTests) {
        alpha_mode = 0;
      } else if (color_control.alpha_to_mask_enable) {
        alpha_mode = 2;
      } else if (color_control.alpha_test_enable &&
                 color_control.alpha_func != xenos::CompareFunction::kAlways) {
        alpha_mode = 1;
      }
      ++use.alpha_modes[alpha_mode];
    }
  }
  submission_in_progress_.last_render_pass_key =
      render_target_cache_->last_update_render_pass_key().key;

  // vulkan_debug_draw_ceiling: take parts of the draw away - the pixels (a 1x1
  // scissor, restored for the next draw), also the vertices past the first
  // primitive, or the draw command itself.
  const int32_t draw_ceiling = cvars::vulkan_debug_draw_ceiling;
  uint32_t host_draw_vertex_count =
      primitive_processing_result.host_draw_vertex_count;
  if (draw_ceiling == 1 || draw_ceiling == 2) {
    const VkRect2D one_pixel = {{0, 0}, {1, 1}};
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &one_pixel);
    dynamic_scissor_update_needed_ = true;
    if (draw_ceiling == 2) {
      host_draw_vertex_count = std::min(host_draw_vertex_count, uint32_t(3));
    }
  }

  // Draw.
  if (draw_ceiling == 3) {
    // Nothing drawn.
  } else if (primitive_processing_result.index_buffer_type ==
                 PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
             shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    const VkIndexType index_type =
        primitive_processing_result.host_index_format ==
                xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32;
    // Guest DMA indices: the buffer stays bound at 0 and the draw starts at
    // the guest index base instead (the base is aligned to the index size).
    uint32_t first_index = 0;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        index_buffer.first = route_to_host ? shared_memory_->host_buffer()
                                           : shared_memory_->buffer();
        index_buffer.second = 0;
        first_index = primitive_processing_result.guest_index_base >>
                      (index_type == VK_INDEX_TYPE_UINT16 ? 1 : 2);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    if (current_index_buffer_ != index_buffer.first ||
        current_index_buffer_offset_ != index_buffer.second ||
        current_index_type_ != index_type) {
      deferred_command_buffer_.CmdVkBindIndexBuffer(
          index_buffer.first, index_buffer.second, index_type);
      current_index_buffer_ = index_buffer.first;
      current_index_buffer_offset_ = index_buffer.second;
      current_index_type_ = index_type;
    }
    deferred_command_buffer_.CmdVkDrawIndexed(host_draw_vertex_count, 1,
                                              first_index, 0, 0);
  }

  // Pop debug marker for draw call.
  PopDebugMarker();

  // Invalidate textures in memexported memory and watch for changes.
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    // read_back: the bytes it changes reach guest memory by the readback -
    // the rest of the range is the CPU's (Need for Speed: Most Wanted's job
    // pool shares a 400 KB export range, of which the draw changes 1 byte).
    shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes,
                                      !route_to_host, memexport_readback);
  }
  if (!memexport_ranges_.empty() &&
      frame_current_ < uint64_t(std::max(cvars::resolve_log_frames, 0))) {
    LogMemexportDraw(*vertex_shader, pixel_shader, index_count,
                     index_buffer_info);
  }

  if (route_to_host && !memexport_ranges_.empty()) {
    // Producer draw: output landed in host_buffer_ (guest RAM), already CPU
    // coherent, so no readback. Just record the written pages. Consumers then
    // route to host_buffer_ (geometry) or copy their own range into the device
    // buffer on demand (texture loads).
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      MarkMemexportPagesWritten(memexport_range.base_address_dwords << 2,
                                 memexport_range.size_bytes);
    }
  } else if (memexport_readback && !memexport_deferred) {
    // (memexport_readback_deferred: read back once the submissions of all
    // pending exports complete - ApplyCompletedMemexportReadbacks.)
    // Android's KGSL drivers may not expose VK_EXT_external_memory_host.
    // In that case the shader wrote the device buffer, not guest RAM. A guest
    // CPU or PM4_WAIT_REG_MEM reader must see the completed export even if no
    // swap/fence packet follows this draw. Use the dense UMA mapping as a
    // synchronous fallback; never publish uncompleted GPU writes to the CPU.
    // Only the bytes the draw changed (against the snapshot taken before it)
    // are stored, byte by byte: the rest of a range is whatever the CPU has,
    // possibly newer than the buffer and written concurrently right now.
    PushBufferMemoryBarrier(
        shared_memory_->buffer(), memexport_extent_start,
        memexport_extent_end - memexport_extent_start,
        guest_shader_pipeline_stages_, VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
    SubmitBarriers(true);
    if (!AwaitAllQueueOperationsCompletion()) {
      XELOGE("Memexport readback: GPU submission did not complete");
      return false;
    }
    ++vk_frame_sync_stats_.memexport_awaits;
    if (!memexport_snapshot_coherent_) {
      const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
      VkMappedMemoryRange snapshot_range;
      snapshot_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      snapshot_range.pNext = nullptr;
      snapshot_range.memory = memexport_snapshot_memory_;
      snapshot_range.offset = 0;
      snapshot_range.size = VK_WHOLE_SIZE;
      vulkan_device->functions().vkInvalidateMappedMemoryRanges(
          vulkan_device->device(), 1, &snapshot_range);
    }
    for (size_t i = 0; i < memexport_ranges_.size(); ++i) {
      if (!StoreMemexportChanges(
              memexport_ranges_[i].base_address_dwords << 2,
              memexport_ranges_[i].size_bytes,
              memexport_snapshot_mapping_ +
                  memexport_snapshot_regions_[i].dstOffset)) {
        return false;
      }
    }
  }

  // Optionally split the frame into multiple submissions (every
  // vulkan_mid_frame_submission_draws real draws) so the GPU starts rendering
  // while the rest of the frame's command stream is still being built, instead
  // of receiving the whole frame at swap time and idling until then. Counted
  // here, at the tail of IssueDraw, so only genuine rasterizing draws are
  // counted (kCopy/empty/no-VS paths return earlier). EndSubmission(false)
  // closes the render pass and submits without closing the frame; the next
  // IssueDraw's BeginSubmission(true) reopens the submission, and the same call
  // resets draws_since_submission_ to 0. Guard mirrors OnPrimaryBufferEnd().
  ++draws_since_submission_;
  ++vk_frame_sync_stats_.draws;
  if (pass_ts_open_pair_ != UINT32_MAX) {
    ++pass_open_draws_;
    pass_open_scissor_w_ =
        std::max(pass_open_scissor_w_,
                 uint32_t(std::max(0, dynamic_scissor_.offset.x)) +
                     dynamic_scissor_.extent.width);
    pass_open_scissor_h_ =
        std::max(pass_open_scissor_h_,
                 uint32_t(std::max(0, dynamic_scissor_.offset.y)) +
                     dynamic_scissor_.extent.height);
    pass_open_viewport_w_ = std::max(
        pass_open_viewport_w_,
        uint32_t(std::max(0.0f, dynamic_viewport_.x + dynamic_viewport_.width)));
    pass_open_viewport_h_ =
        std::max(pass_open_viewport_h_,
                 uint32_t(std::max(0.0f, dynamic_viewport_.y +
                                             std::abs(dynamic_viewport_.height))));
  }
  uint32_t submission_draws =
      uint32_t(std::max(cvars::vulkan_mid_frame_submission_draws, 0));
  if (cvars::vulkan_drained_submission_draws > 0 &&
      GetCurrentSubmission() < drained_submission_ + 16) {
    // Right after the GPU was drained: it waits for this submission.
    const uint32_t drained_draws =
        uint32_t(cvars::vulkan_drained_submission_draws);
    submission_draws = submission_draws
                           ? std::min(submission_draws, drained_draws)
                           : drained_draws;
  }
  if (submission_draws && draws_since_submission_ >= submission_draws &&
      submission_open_ && !scratch_buffer_used_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }

  return true;
}

bool VulkanCommandProcessor::EnsureMemexportRangeInDeviceBuffer(
    uint32_t base_bytes, uint32_t size_bytes) {
  // Readers of the device buffer need memexport output copied across from
  // host_buffer_ (guest RAM), where it actually lives. Doing it on the GPU
  // keeps it ordered against the writes that produced it, which a CPU read
  // cannot be.
  if (shared_memory_host_and_edram_descriptor_set_ == VK_NULL_HANDLE ||
      !size_bytes || base_bytes >= SharedMemory::kBufferSize) {
    return false;
  }
  size_bytes = std::min(size_bytes, SharedMemory::kBufferSize - base_bytes);
  if (!IsMemexportRange(base_bytes, size_bytes)) {
    return false;
  }
  VkBuffer host_buffer = shared_memory_->host_buffer();
  VkBuffer device_buffer = shared_memory_->buffer();
  if (host_buffer == VK_NULL_HANDLE) {
    return false;
  }

  const VkPipelineStageFlags read_stages =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkAccessFlags read_access = VK_ACCESS_INDEX_READ_BIT |
                                    VK_ACCESS_SHADER_READ_BIT |
                                    VK_ACCESS_TRANSFER_READ_BIT;

  // Order the writes on host_buffer_ before the transfer read (the producers
  // may have run several draws ago, so barrier host_buffer_ explicitly rather
  // than relying on Use's last-usage tracking), and prior device-buffer reads
  // before the transfer write into it. Resolves write host_buffer_ by transfer,
  // not just memexport shaders, so both source masks are needed.
  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  PushBufferMemoryBarrier(
      host_buffer, VkDeviceSize(base_bytes), VkDeviceSize(size_bytes),
      guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_TRANSFER_READ_BIT);
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes), read_stages,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_access,
                          VK_ACCESS_TRANSFER_WRITE_BIT);
  SubmitBarriers(true);

  VkBufferCopy copy_region;
  copy_region.srcOffset = base_bytes;
  copy_region.dstOffset = base_bytes;
  copy_region.size = size_bytes;
  deferred_command_buffer_.CmdVkCopyBuffer(host_buffer, device_buffer, 1,
                                           &copy_region);

  // Make the copied data visible to the following read.
  PushBufferMemoryBarrier(device_buffer, VkDeviceSize(base_bytes),
                          VkDeviceSize(size_bytes),
                          VK_PIPELINE_STAGE_TRANSFER_BIT, read_stages,
                          VK_ACCESS_TRANSFER_WRITE_BIT, read_access);
  return true;
}

void VulkanCommandProcessor::ResolveReadCallbackThunk(void* context,
                                                      uint32_t physical_address,
                                                      uint32_t length) {
  static_cast<VulkanCommandProcessor*>(context)->MarkResolvePagesRead(
      physical_address, length);
}

uint32_t VulkanCommandProcessor::GetMsaa4xDepthSurface() const {
  const RegisterFile& regs = *register_file_;
  return regs.Get<reg::RB_DEPTH_INFO>().depth_base |
         (uint32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch) << 16);
}

bool VulkanCommandProcessor::RewriteMsaa4xSurfaceInfoForDraw(
    const Shader* pixel_shader, bool pixel_shader_uses_position) {
  if (!cvars::vulkan_depth_4x_as_1x ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kHostRenderTargets) {
    return false;
  }
  const RegisterFile& regs = *register_file_;
  uint32_t& surface_info_value =
      register_file_->values[XE_GPU_REG_RB_SURFACE_INFO];
  reg::RB_SURFACE_INFO surface_info;
  surface_info.value = surface_info_value;
  if (surface_info.msaa_samples != xenos::MsaaSamples::k4X) {
    return false;
  }
  // The scene classification depends on which pixel shaders may run per 1x
  // pixel.
  if (msaa_4x_scene_depth_surfaces_simple_ps_ !=
      cvars::vulkan_samples_as_pixels_simple_ps) {
    msaa_4x_scene_depth_surfaces_simple_ps_ =
        cvars::vulkan_samples_as_pixels_simple_ps;
    msaa_4x_scene_depth_surfaces_.clear();
  }
  // A pixel shader whose result doesn't depend on running per 4x pixel rather
  // than per 1x pixel beyond the interpolation point: no textures or
  // gradients (implicit derivatives would halve), no position, no memory
  // export, no alpha to coverage (no samples at 1x).
  const bool pixel_shader_simple =
      pixel_shader && cvars::vulkan_samples_as_pixels_simple_ps &&
      !pixel_shader->uses_texture_fetch_instruction_results() &&
      !pixel_shader_uses_position && !pixel_shader->memexport_eM_written() &&
      !regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable;
  const uint32_t color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(
                         regs, pixel_shader->writes_color_targets())
                   : 0;
  // A depth surface drawn with color at least once (by a pixel shader that
  // must run per 4x pixel) is a multisampled scene's. The others are depth-
  // only or stencil marking - titles reinterpret their samples as the pixels
  // of a 1x surface twice the size (Forza Horizon's shadow atlas, and the
  // quarter-resolution stencil marking of its 1280x720 lighting).
  const uint32_t depth_surface = GetMsaa4xDepthSurface();
  bool scene;
  if (color_mask && !pixel_shader_simple) {
    msaa_4x_scene_depth_surfaces_.insert(depth_surface);
    scene = true;
  } else {
    scene = msaa_4x_scene_depth_surfaces_.count(depth_surface) != 0;
  }
  if (scene) {
    return false;
  }
  // Depth-only draws (no pixel shader) or simple pixel shaders - nothing then
  // depends on the sample positions or on the pixel shader seeing 4x.
  if ((pixel_shader && !pixel_shader_simple) ||
      texture_cache_->IsDrawResolutionScaled() ||
      uint32_t(surface_info.surface_pitch) * 2 >
          xenos::kTexture2DCubeMaxWidthHeight) {
    return false;
  }
  // Over EDRAM 2x MSAA surfaces of the same pitch own, the 2x surface twice
  // as wide (vulkan_samples_as_pixels_2x) - 2 vertical samples per pixel at
  // both 4x and 2x.
  bool keep_vertical_samples = false;
  if (cvars::vulkan_samples_as_pixels_2x) {
    const uint32_t pitch_tiles_at_32bpp =
        ((uint32_t(surface_info.surface_pitch) << 1) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples;
    uint32_t bases_checked = 0;
    bool all_owned_by_2x = true;
    auto check_base = [&](uint32_t base_tiles) {
      ++bases_checked;
      xenos::MsaaSamples owner_msaa_samples;
      uint32_t owner_pitch_tiles_at_32bpp;
      if (!render_target_cache_->GetEdramTileOwner(
              base_tiles, owner_msaa_samples, owner_pitch_tiles_at_32bpp) ||
          owner_msaa_samples != xenos::MsaaSamples::k2X ||
          owner_pitch_tiles_at_32bpp != pitch_tiles_at_32bpp) {
        all_owned_by_2x = false;
      }
    };
    const auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
    if (depth_control.z_enable || depth_control.stencil_enable) {
      check_base(regs.Get<reg::RB_DEPTH_INFO>().depth_base);
    }
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if ((color_mask >> (4 * i)) & 0b1111) {
        check_base(regs.Get<reg::RB_COLOR_INFO>(
                           reg::RB_COLOR_INFO::rt_register_indices[i])
                       .color_base);
      }
    }
    keep_vertical_samples = bases_checked && all_owned_by_2x;
  }
  // The same EDRAM tiles as the 1x surface twice as wide (and tall): 2
  // horizontal samples per pixel at 4x, 1 at 1x.
  surface_info.msaa_samples = keep_vertical_samples ? xenos::MsaaSamples::k2X
                                                    : xenos::MsaaSamples::k1X;
  surface_info.surface_pitch = surface_info.surface_pitch * 2;
  surface_info_value = surface_info.value;
  render_target_cache_->SetDrawSamplesAsPixels(true, keep_vertical_samples);
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kResolve);

  if (!BeginSubmission(true)) {
    return false;
  }
  ++submission_work_.resolves;

  // Push debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PushDebugMarker("IssueCopy (Resolve)");
  }

  // Bracket the resolve emission region with GPU timestamps (BOTTOM_OF_PIPE:
  // the region begins and ends at full barriers already, so the wait-for-idle
  // Turnip implies is ~free here).
  uint32_t resolve_ts_pair = UINT32_MAX;
  if (resolve_timestamp_mapping_) {
    if (resolve_ts_submission_ != GetCurrentSubmission()) {
      resolve_ts_submission_ = GetCurrentSubmission();
      resolve_ts_count_ = 0;
    }
    if (resolve_ts_count_ < kResolveTimestampPairsPerSubmission) {
      resolve_ts_pair =
          uint32_t(resolve_ts_submission_ % kResolveTimestampRingSubmissions) *
              kResolveTimestampPairsPerSubmission +
          resolve_ts_count_;
      deferred_command_buffer_.CmdVkWriteTimestamp(
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resolve_timestamp_pool_,
          resolve_ts_pair * kResolveTimestampsPerResolve);
      resolve_ts_open_ = resolve_ts_pair;
      resolve_ts_copy_end_written_ = false;
    } else {
      ++vk_frame_sync_stats_.resolve_ts_dropped;
    }
  }

  uint32_t written_address, written_length;
  reg::RB_COPY_DEST_INFO copy_dest_info;
  bool is_scaled;
  // merge_tiling_bands: a later band is resolved from its rows.
  render_target_cache_->SetTilingBandResolveRows(
      tiling_band_merge_active() && tiling_band() > 0 &&
              tiling_band_sequence_seen_ == tiling_band_sequence()
          ? uint32_t(tiling_band()) * tiling_band_rows_
          : 0);
  const bool resolve_succeeded = render_target_cache_->Resolve(
      *memory_, *shared_memory_, *texture_cache_, written_address,
      written_length, &copy_dest_info, &is_scaled);
  render_target_cache_->SetTilingBandResolveRows(0);
  if (resolve_ts_pair != UINT32_MAX) {
    // Always write all three of an opened resolve - a WAIT_BIT results copy
    // over an unwritten query would hang the GPU. Without a copy end marked
    // by the render target cache, the whole resolve counts as the copy.
    MarkResolveCopyEnd();
    resolve_ts_open_ = UINT32_MAX;
    deferred_command_buffer_.CmdVkWriteTimestamp(
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resolve_timestamp_pool_,
        resolve_ts_pair * kResolveTimestampsPerResolve + 2);
    resolve_ts_keys_[resolve_ts_pair] = render_target_cache_->last_resolve_key();
    ++resolve_ts_count_;
  }
  if (!resolve_succeeded) {
    if (debug_markers_enabled_) {
      PopDebugMarker();
    }
    return false;
  }
  ++submission_in_progress_.resolve_count;
  ++vk_frame_sync_stats_.resolves;
  resolve_since_pass_end_ = true;
  if (frame_dump_file_) {
    DumpFrameResolve(written_address, written_length);
  }
  if (frame_current_ < uint64_t(std::max(cvars::resolve_log_frames, 0))) {
    LogResolve(written_address, written_length);
  }

  // The resolve wrote the device buffer. Drop any stale memexport marks so the
  // output isn't overwritten with guest RAM by a later texture load.
  ClearMemexportPages(written_address, written_length);

  ReadbackResolveMode readback_mode = GetReadbackResolveMode();
  const bool zero_copy = shared_memory_->is_zero_copy();
  // Readback lands in guest RAM: the host buffer in two-buffer mode, or buffer_
  // itself in zero-copy mode, since it already aliases guest RAM.
  VkBuffer resolve_host_buffer =
      zero_copy ? shared_memory_->buffer() : shared_memory_->host_buffer();
  // uma reads the host mapping directly, so it needs no imported buffer - the
  // copy branches below check resolve_host_buffer themselves. Gating the whole
  // block on it disabled readback outright wherever guest RAM cannot be
  // imported (no VK_EXT_external_memory_host, i.e. every Adreno).
  const bool uma_direct = readback_mode == ReadbackResolveMode::kUma &&
                          !texture_cache_->IsDrawResolutionScaled() &&
                          shared_memory_->IsHostMapped();
  if (readback_mode != ReadbackResolveMode::kDisabled && written_length > 0 &&
      resolve_host_buffer == VK_NULL_HANDLE && !uma_direct) {
    static bool readback_unavailable_logged = false;
    if (!readback_unavailable_logged) {
      readback_unavailable_logged = true;
      XELOGW(
          "Resolve readback is enabled but guest RAM is neither host-imported "
          "nor host-mapped - readback will not happen");
    }
  }
  if (readback_mode != ReadbackResolveMode::kDisabled && written_length > 0 &&
      (resolve_host_buffer != VK_NULL_HANDLE || uma_direct) &&
      IsResolveDestinationResident(written_address, written_length)) {
    bool stall_after_copy;
    if (!DecideResolveHostCopy(readback_mode, written_address, written_length,
                               cvars::readback_resolve_sync,
                               stall_after_copy)) {
      // some mode: the range has not been read since its last resolve.
      PopDebugMarker();
      return true;
    }

    // UMA: shared memory is host-mapped, so the CPU reads the resolved bytes
    // straight out of it - no device->host copy at all. Serves the same role as
    // upstream's zero_copy on devices that cannot import guest RAM. Kept
    // selectable against kFast/kAll so the two designs can be compared.
    if (uma_direct) {
      ++vk_frame_sync_stats_.rb_uma_direct;
      const uint64_t resolve_submission = GetCurrentSubmission();
      const uint64_t resolve_key =
          MakeReadbackResolveKey(written_address, written_length);
      auto& last_write = uma_readback_last_write_[resolve_key];
      // readback_resolve_uma_read_watch: a range seeded before is copied
      // again only once the CPU has read it.
      if (cvars::readback_resolve_uma_read_watch &&
          written_length >=
              uint32_t(std::max(
                  cvars::readback_resolve_uma_read_watch_min_kb, 0)) *
                  1024) {
        const bool first_watched = TryArmResolveReadWatch(
            resolve_key, written_address, written_length);
        if (!first_watched && last_write &&
            !AnyResolvePageRead(written_address, written_length)) {
          ++vk_frame_sync_stats_.rb_uma_unread;
          if (cvars::readback_resolve_uma_unread_traffic) {
            uma_unread_traffic_buffer_.resize(written_length);
            shared_memory_->ReadHostMapped(written_address, written_length,
                                           uma_unread_traffic_buffer_.data());
          }
          last_write = resolve_submission;
          PopDebugMarker();
          return true;
        }
      }
      bool do_read;
      if (!last_write) {
        ++vk_frame_sync_stats_.rb_uma_first_use;
        // First resolve of this destination: guest RAM holds nothing yet, so
        // make the just-recorded write host-visible and drain to read it now.
        // Guest shader stages included: in-pass resolves write shared memory
        // from the fragment stage, not just compute/transfer.
        PushBufferMemoryBarrier(
            shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
            guest_shader_pipeline_stages_ |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, false);
        SubmitBarriers(true);
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE("VulkanCommandProcessor: uma resolve readback drain failed");
          PopDebugMarker();
          return true;
        }
        do_read = true;
      } else {
        // Read the previous resolve's bytes, but only once that submission has
        // retired, so the CPU never races the GPU writing the same region.
        // Cached completed-submission value - no blocking poll (vkGetFenceStatus
        // stalls on Turnip). Staleness is bounded by frames-in-flight.
        do_read = GetCompletedSubmission() >= last_write;
        if (!do_read &&
            ui::vulkan::VulkanGPUCompletionTimeline::bounded_collection() &&
            last_write < GetCurrentSubmission() &&
            frame_current_ - uma_readback_last_read_frame_[resolve_key] >=
                kMaxFramesInFlight) {
          // With bounded fence collection (debug.xendroid.fence_collect), the
          // completed submission only reaches the frame throttle target, which
          // a destination resolved every frame is always newer than - without
          // this, guest RAM would keep its first readback for good. Wait for
          // the newest write once it has gone that many frames unread.
          CheckSubmissionCompletionAndDeviceLoss(last_write);
          do_read = GetCompletedSubmission() >= last_write;
        }
      }
      if (do_read) {
        if (ui::vulkan::VulkanGPUCompletionTimeline::bounded_collection()) {
          uma_readback_last_read_frame_[resolve_key] = frame_current_;
        }
        InsertDebugMarker("Resolve Readback (uma): 0x%08X, %u bytes",
                          written_address, written_length);
        vk_frame_sync_stats_.rb_uma_bytes += written_length;
        shared_memory_->ReadHostMapped(
            written_address, written_length,
            memory_->TranslatePhysical(written_address));
        shared_memory_->NoteGuestCopy(
            written_address, written_length,
            SharedMemory::GuestCopySource::kResolveReadback);
      }
      last_write = resolve_submission;
      PopDebugMarker();
      return true;
    }

    if (!texture_cache_->IsDrawResolutionScaled()) {
      if (zero_copy) {
        // The non-scaled resolve already wrote buffer_ (guest RAM) in place, so
        // it is coherent with the CPU - nothing to read back.
        PopDebugMarker();
        return true;
      }
      // Non-scaled: copy the resolved range straight from the device buffer
      // into host_buffer_ (guest RAM).
      VkBuffer resolve_device_buffer = shared_memory_->buffer();
      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      // Order prior memexport and resolve writes to host_buffer_ before this
      // copy.
      PushBufferMemoryBarrier(
          resolve_host_buffer, VkDeviceSize(written_address),
          VkDeviceSize(written_length),
          guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT);
      SubmitBarriers(true);
      InsertDebugMarker("Resolve Sync (guest RAM): 0x%08X, %u bytes",
                        written_address, written_length);
      VkBufferCopy copy_region = {};
      copy_region.srcOffset = written_address;
      copy_region.dstOffset = written_address;
      copy_region.size = written_length;
      deferred_command_buffer_.CmdVkCopyBuffer(
          resolve_device_buffer, resolve_host_buffer, 1, &copy_region);
      // Make the copy visible to within-frame consumers reading host_buffer_
      // (route_to_host draws sampling guest RAM as index, vertex or texture,
      // and EnsureMemexportRangeInDeviceBuffer copying out of it). Use()'s
      // mirror barrier is keyed on the device buffer, so it does not cover this
      // transfer write.
      PushBufferMemoryBarrier(
          resolve_host_buffer, VkDeviceSize(written_address),
          VkDeviceSize(written_length), VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
              VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
      if (stall_after_copy) {
        // host_buffer_ is HOST_COHERENT, so waiting makes the copy visible to
        // the guest CPU before a later read races it.
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE(
              "VulkanCommandProcessor: Failed to complete queue operations for "
              "resolve sync");
        }
      }
      PopDebugMarker();
      return true;
    }
    // Scaled: GPU compute downscale into host_buffer_ (guest RAM).
    // Get scaled resolve buffer (works for both sparse and simple buffer modes)
    VkBuffer scaled_buffer = texture_cache_->GetCurrentScaledResolveBuffer();
    if (scaled_buffer == VK_NULL_HANDLE) {
      XELOGE("VulkanCommandProcessor: No scaled resolve buffer available");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    uint32_t scale_x = texture_cache_->draw_resolution_scale_x();
    uint32_t scale_y = texture_cache_->draw_resolution_scale_y();
    uint32_t scale_area = scale_x * scale_y;

    assert_true(scale_x >= 1 &&
                scale_x <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
    assert_true(scale_y >= 1 &&
                scale_y <= TextureCache::kMaxDrawResolutionScaleAlongAxis);
    assert_true(scale_x > 1 || scale_y > 1);

    // Texel size from the normalized copy_dest_info, not a re-read of
    // RB_COPY_DEST_INFO - for depth the register can hold a different size.
    uint32_t pixel_size_log2 =
        draw_util::GetResolveDownscalePixelSizeLog2(copy_dest_info);
    if (pixel_size_log2 > 3) {
      // 128bpp - the tiled scaled addressing reversal in the downscale shader
      // does not handle it.
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to a 128bpp "
          "destination - not supported by the downscale shader");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    // The scaled addressing is periodic per guest group, so the written extent
    // must be group-aligned for the per-tile reversal to be valid.
    uint32_t group_bytes_log2 = pixel_size_log2 <= 2 ? 7 : 6;
    if (written_address & ((uint32_t(1) << group_bytes_log2) - 1)) {
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
          "destination is not aligned to the scaled addressing group size",
          written_address);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    uint32_t tile_size_1x = (32u * 32u) << pixel_size_log2;
    uint32_t tile_count = written_length / tile_size_1x;
    if (tile_count == 0) {
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    // Only whole 32x32 tiles are downscaled - truncate a partial tail so stale
    // data is not copied to the guest.
    uint32_t readback_length = tile_count * tile_size_1x;

    // Bind the source at the written extent (the range starts earlier, at the
    // destination base) and skip if the extent isn't fully inside the range.
    uint64_t scaled_start = uint64_t(written_address) * scale_area;
    uint64_t scaled_readback_length = uint64_t(readback_length) * scale_area;
    uint64_t range_start_scaled =
        texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    uint64_t range_length_scaled =
        texture_cache_->GetCurrentScaledResolveRangeLengthScaled();
    if (!range_length_scaled || scaled_start < range_start_scaled ||
        scaled_start + scaled_readback_length >
            range_start_scaled + range_length_scaled) {
      XELOGGPU(
          "Skipping readback of a resolution-scaled resolve to 0x{:08X} - the "
          "written extent is not within the current scaled resolve range",
          written_address);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Calculate offset within the buffer using the buffer's base address.
    // GetCurrentScaledResolveBufferBaseOffset() returns:
    // - For sparse buffers: buffer_index << 30 (same as D3D12's 1GB chunks)
    // - For simple buffers: the buffer's range_start_scaled
    uint64_t buffer_base =
        texture_cache_->GetCurrentScaledResolveBufferBaseOffset();
    if (scaled_start < buffer_base) {
      XELOGE(
          "VulkanCommandProcessor: Scaled address {} is before buffer start {}",
          scaled_start, buffer_base);
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }
    uint64_t source_offset = scaled_start - buffer_base;

    // The downscale compute writes 1x data straight into host_buffer_ (guest
    // RAM) at the resolved range.
    VkBuffer dest_buffer = resolve_host_buffer;
    VkDeviceSize dest_offset = written_address;

    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    // Ensure intermediate buffer for GPU downscaling is large enough
    uint32_t downscale_buffer_size = AlignReadbackBufferSize(readback_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      // Clean up old buffer
      if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
        if (!AwaitAllQueueOperationsCompletion()) {
          XELOGE(
              "VulkanCommandProcessor: Failed to wait for GPU before "
              "destroying old downscale buffer");
          if (debug_markers_enabled_) {
            PopDebugMarker();
          }
          return true;
        }
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_size_ = 0;
      }

      VkBufferCreateInfo buffer_info = {};
      buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      buffer_info.size = downscale_buffer_size;
      buffer_info.usage =
          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

      if (dfn.vkCreateBuffer(device, &buffer_info, nullptr,
                             &resolve_downscale_buffer_) != VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to create {} MB downscale buffer",
            downscale_buffer_size >> 20);
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      VkMemoryRequirements memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, resolve_downscale_buffer_,
                                        &memory_requirements);

      const uint32_t memory_type_index = ui::vulkan::util::ChooseMemoryType(
          vulkan_device->memory_types(), memory_requirements.memoryTypeBits,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal);

      if (memory_type_index == UINT32_MAX) {
        XELOGE(
            "VulkanCommandProcessor: Failed to find memory type for downscale "
            "buffer");
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      VkMemoryAllocateInfo memory_info = {};
      memory_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      memory_info.allocationSize = memory_requirements.size;
      memory_info.memoryTypeIndex = memory_type_index;

      if (dfn.vkAllocateMemory(device, &memory_info, nullptr,
                               &resolve_downscale_buffer_memory_) !=
          VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to allocate downscale buffer "
            "memory");
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      if (dfn.vkBindBufferMemory(device, resolve_downscale_buffer_,
                                 resolve_downscale_buffer_memory_,
                                 0) != VK_SUCCESS) {
        XELOGE(
            "VulkanCommandProcessor: Failed to bind downscale buffer memory");
        dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        resolve_downscale_buffer_ = VK_NULL_HANDLE;
        resolve_downscale_buffer_memory_ = VK_NULL_HANDLE;
        if (debug_markers_enabled_) {
          PopDebugMarker();
        }
        return true;
      }

      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    // Allocate descriptor set for source and destination buffers.
    // Uses pool chain to avoid mid-frame GPU stalls on pool exhaustion.
    VkDescriptorSet descriptor_set =
        resolve_downscale_descriptor_pool_chain_->Allocate(
            GetCurrentSubmission());
    if (descriptor_set == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanCommandProcessor: Failed to allocate resolve downscale "
          "descriptor set from pool chain");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Ensure submission is open
    if (!BeginSubmission(true)) {
      XELOGE(
          "VulkanCommandProcessor: Failed to begin submission for scaled "
          "resolve readback");
      if (debug_markers_enabled_) {
        PopDebugMarker();
      }
      return true;
    }

    // Update descriptor set with buffer bindings
    // Bind source buffer at offset 0 to avoid alignment issues - offset is
    // passed via push constants and applied in the shader
    std::array<VkDescriptorBufferInfo, 2> buffer_infos;
    buffer_infos[0].buffer = scaled_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = VK_WHOLE_SIZE;
    // Destination buffer (intermediate device-local buffer)
    buffer_infos[1].buffer = resolve_downscale_buffer_;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = readback_length;

    std::array<VkWriteDescriptorSet, 2> descriptor_writes;
    descriptor_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[0].pNext = nullptr;
    descriptor_writes[0].dstSet = descriptor_set;
    descriptor_writes[0].dstBinding = 0;
    descriptor_writes[0].dstArrayElement = 0;
    descriptor_writes[0].descriptorCount = 1;
    descriptor_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[0].pImageInfo = nullptr;
    descriptor_writes[0].pBufferInfo = &buffer_infos[0];
    descriptor_writes[0].pTexelBufferView = nullptr;

    descriptor_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_writes[1].pNext = nullptr;
    descriptor_writes[1].dstSet = descriptor_set;
    descriptor_writes[1].dstBinding = 1;
    descriptor_writes[1].dstArrayElement = 0;
    descriptor_writes[1].descriptorCount = 1;
    descriptor_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_writes[1].pImageInfo = nullptr;
    descriptor_writes[1].pBufferInfo = &buffer_infos[1];
    descriptor_writes[1].pTexelBufferView = nullptr;

    dfn.vkUpdateDescriptorSets(device, uint32_t(descriptor_writes.size()),
                               descriptor_writes.data(), 0, nullptr);

    // End any active render pass and submit barriers
    SubmitBarriers(true);

    // Barrier for source buffer - ensure resolve copy compute shader writes
    // are complete before readback compute shader reads.
    VkBufferMemoryBarrier source_barrier = {};
    source_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    source_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    source_barrier.buffer = scaled_buffer;
    source_barrier.offset = 0;
    source_barrier.size = VK_WHOLE_SIZE;
    deferred_command_buffer_.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &source_barrier,
        0, nullptr);

    PushDebugMarker("Resolve Downscale: 0x%08X, %u bytes -> %u bytes",
                    written_address, uint32_t(scaled_readback_length),
                    readback_length);

    // Bind compute pipeline
    BindExternalComputePipeline(resolve_downscale_pipeline_);

    // Push constants
    ResolveDownscaleConstants constants;
    constants.scale_x = scale_x;
    constants.scale_y = scale_y;
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = static_cast<uint32_t>(source_offset);
    // Optionally sample from center of scaled block instead of top-left.
    constants.half_pixel_offset = (cvars::readback_resolve_half_pixel_offset &&
                                   (scale_x > 1 || scale_y > 1))
                                      ? 1
                                      : 0;
    deferred_command_buffer_.CmdVkPushConstants(
        resolve_downscale_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(constants), &constants);

    // Bind descriptor set
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_COMPUTE, resolve_downscale_pipeline_layout_, 0,
        1, &descriptor_set, 0, nullptr);

    // Dispatch compute shader - one thread group per 32x32 tile
    ++submission_in_progress_.dispatch_count;
    deferred_command_buffer_.CmdVkDispatch(tile_count, 1, 1);

    // Barriers before copy: downscale compute-write -> transfer-read, and order
    // prior writes to host_buffer_ (memexport shader writes and earlier resolve
    // copies) before this copy.
    VkBufferMemoryBarrier pre_copy_barriers[2] = {};
    pre_copy_barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_copy_barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre_copy_barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    pre_copy_barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[0].buffer = resolve_downscale_buffer_;
    pre_copy_barriers[0].offset = 0;
    pre_copy_barriers[0].size = readback_length;
    pre_copy_barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_copy_barriers[1].srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    pre_copy_barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    pre_copy_barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_copy_barriers[1].buffer = dest_buffer;
    pre_copy_barriers[1].offset = dest_offset;
    pre_copy_barriers[1].size = readback_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
            guest_shader_pipeline_stages_,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 2, pre_copy_barriers, 0,
        nullptr);

    // Copy the downscaled data into host_buffer_ (guest RAM).
    VkBufferCopy copy_region = {};
    copy_region.srcOffset = 0;
    copy_region.dstOffset = dest_offset;
    copy_region.size = readback_length;
    deferred_command_buffer_.CmdVkCopyBuffer(resolve_downscale_buffer_,
                                             dest_buffer, 1, &copy_region);
    // Make the copy visible to within-frame consumers reading host_buffer_
    // (route_to_host draws sampling guest RAM as index, vertex or texture, and
    // EnsureMemexportRangeInDeviceBuffer copying out of it).
    PushBufferMemoryBarrier(
        dest_buffer, dest_offset, VkDeviceSize(readback_length),
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_ |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);

    PopDebugMarker();

    if (stall_after_copy) {
      // host_buffer_ is HOST_COHERENT, so waiting makes the copy visible to the
      // guest CPU before a later read races it.
      if (!AwaitAllQueueOperationsCompletion()) {
        XELOGE(
            "VulkanCommandProcessor: Failed to complete queue operations for "
            "scaled resolve sync");
      }
    }
  }

  // Pop debug marker for resolve operation.
  if (debug_markers_enabled_) {
    PopDebugMarker();
  }

  return true;
}

void VulkanCommandProcessor::EnsureZPDQueryResources() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  bool can_recreate =
      !zpd_active_segment_.logical_active &&
      !zpd_active_segment_.segment_active &&
      zpd_active_query_index_ == UINT32_MAX && !zpd_active_query_is_fsi_ &&
      !zpd_host_query_pool_->has_pending_resolve_batch() &&
      zpd_resolves_in_flight_.empty() && zpd_deferred_releases_.empty();

  bool initialize_fsi_counter = render_target_cache_->GetPath() ==
                                RenderTargetCache::Path::kPixelShaderInterlock;

  zpd_host_query_pool_->EnsureInitialized(GetVulkanDevice(),
                                          kZPDQueryPoolCapacity, can_recreate,
                                          initialize_fsi_counter);

  if (initialize_fsi_counter &&
      zpd_host_query_pool_->fsi_counter_initialized()) {
    VkBuffer fsi_counter_buffer = zpd_host_query_pool_->fsi_counter_buffer();
    VkDeviceSize fsi_counter_range =
        sizeof(uint32_t) * zpd_host_query_pool_->capacity();
    if (zpd_fsi_counter_descriptor_buffer_ != fsi_counter_buffer ||
        zpd_fsi_counter_descriptor_range_ != fsi_counter_range) {
      VkDescriptorBufferInfo fsi_counter_descriptor_buffer_info;
      fsi_counter_descriptor_buffer_info.buffer = fsi_counter_buffer;
      fsi_counter_descriptor_buffer_info.offset = 0;
      fsi_counter_descriptor_buffer_info.range = fsi_counter_range;

      // Mirror binding 2 to the host-imported routing set too, so a routed FSI
      // draw reads the same counter.
      VkWriteDescriptorSet fsi_counter_descriptor_writes[2];
      uint32_t fsi_counter_descriptor_write_count = 0;
      for (VkDescriptorSet set :
           {shared_memory_and_edram_descriptor_set_,
            shared_memory_host_and_edram_descriptor_set_}) {
        if (set == VK_NULL_HANDLE) {
          continue;
        }
        VkWriteDescriptorSet& fsi_counter_descriptor_write =
            fsi_counter_descriptor_writes[fsi_counter_descriptor_write_count++];
        fsi_counter_descriptor_write.sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fsi_counter_descriptor_write.pNext = nullptr;
        fsi_counter_descriptor_write.dstSet = set;
        fsi_counter_descriptor_write.dstBinding = 2;
        fsi_counter_descriptor_write.dstArrayElement = 0;
        fsi_counter_descriptor_write.descriptorCount = 1;
        fsi_counter_descriptor_write.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fsi_counter_descriptor_write.pImageInfo = nullptr;
        fsi_counter_descriptor_write.pBufferInfo =
            &fsi_counter_descriptor_buffer_info;
        fsi_counter_descriptor_write.pTexelBufferView = nullptr;
      }

      const ui::vulkan::VulkanDevice::Functions& dfn =
          GetVulkanDevice()->functions();
      dfn.vkUpdateDescriptorSets(GetVulkanDevice()->device(),
                                 fsi_counter_descriptor_write_count,
                                 fsi_counter_descriptor_writes, 0, nullptr);

      zpd_fsi_counter_descriptor_buffer_ = fsi_counter_buffer;
      zpd_fsi_counter_descriptor_range_ = fsi_counter_range;
    }
  } else if (!IsZPDQueryPoolReady() && cvars::occlusion_query_log) {
    XELOGI(
        "ZPD/Vulkan: FSI counter resources unavailable; keeping counter index "
        "sentinel active");
  }
  zpd_fsi_counter_index_force_update_ = true;
}

bool VulkanCommandProcessor::IsZPDQueryPoolReady() const {
  if (!zpd_host_query_pool_) {
    return false;
  }
  if (!render_target_cache_ ||
      render_target_cache_->GetPath() !=
          RenderTargetCache::Path::kPixelShaderInterlock) {
    return zpd_host_query_pool_->is_initialized();
  }
  VkDeviceSize fsi_counter_range =
      sizeof(uint32_t) * zpd_host_query_pool_->capacity();
  return zpd_host_query_pool_->fsi_counter_initialized() &&
         zpd_fsi_counter_descriptor_buffer_ ==
             zpd_host_query_pool_->fsi_counter_buffer() &&
         zpd_fsi_counter_descriptor_range_ == fsi_counter_range;
}

bool VulkanCommandProcessor::CanOpenZPDQuery() const {
  if (!submission_open_) {
    return false;
  }
  bool use_fsi_counter_path =
      render_target_cache_ &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kPixelShaderInterlock;
  return use_fsi_counter_path || in_render_pass_;
}

CommandProcessor::QueryOpenResult VulkanCommandProcessor::OpenZPDQuery(
    ReportHandle report_handle, bool can_close_submission) {
  bool use_fsi_counter_path =
      zpd_host_query_pool_->fsi_counter_initialized() &&
      render_target_cache_->GetPath() ==
          RenderTargetCache::Path::kPixelShaderInterlock;

  if (!BeginSubmission(true)) {
    return QueryOpenResult::kFailed;
  }

  if (!use_fsi_counter_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  bool retried_after_submission_flip = false;
  while (true) {
    bool is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();

    if (is_pool_exhausted) {
      PumpQueryResolves();
      is_pool_exhausted = !zpd_host_query_pool_->has_free_indices();
    }

    if (is_pool_exhausted &&
        (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt)) {
      return QueryOpenResult::kPoolExhausted;
    }

    uint64_t wait_for = 0;
    if (is_pool_exhausted && !zpd_resolves_in_flight_.empty()) {
      wait_for = zpd_resolves_in_flight_.front().submission;
    }

    if (wait_for == 0) {
      break;
    }

    if (submission_open_ && wait_for == GetCurrentSubmission()) {
      if (retried_after_submission_flip || !can_close_submission ||
          !CanEndSubmissionImmediately()) {
        return QueryOpenResult::kDeferred;
      }

      VkRenderPass saved_render_pass = VK_NULL_HANDLE;
      const VulkanRenderTargetCache::Framebuffer* saved_framebuffer = nullptr;
      if (in_render_pass_) {
        saved_render_pass = current_render_pass_;
        saved_framebuffer = current_framebuffer_;
        PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kQuery);
        EndRenderPass();
      }
      if (!EndSubmission(false)) {
        return QueryOpenResult::kFailed;
      }
      if (!BeginSubmission(true)) {
        return QueryOpenResult::kFailed;
      }

      if (saved_framebuffer) {
        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        zpd_active_segment_.segment_pending_begin = false;
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
        if (!in_render_pass_) {
          return QueryOpenResult::kDeferred;
        }
      }

      retried_after_submission_flip = true;
      continue;
    }

    uint64_t completed_submission = GetCompletedSubmission();
    if (wait_for > completed_submission) {
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD: Stall awaiting submission={} completed_before={}",
               wait_for, completed_submission);
      }
      completion_timeline_.AwaitSubmissionAndUpdateCompleted(wait_for);
      PumpQueryResolves();
    }

    break;
  }

  if (!use_fsi_counter_path && !in_render_pass_) {
    return QueryOpenResult::kDeferred;
  }

  if (!zpd_host_query_pool_->AcquireQueryIndex(zpd_active_query_index_,
                                               zpd_active_query_generation_)) {
    return QueryOpenResult::kFailed;
  }

  zpd_active_query_is_fsi_ = use_fsi_counter_path;

  // FSI queries don't use Vulkan occlusion queries at all.
  // While the segment is open, the translated pixel shader accumulates passed
  // MSAA samples into one counter slot selected via zpd_fsi_counter_index.
  // Clear the slot here so a recycled index never inherits old counts.
  if (zpd_active_query_is_fsi_) {
    bool fsi_counter_cleared = false;
    if (zpd_host_query_pool_->fsi_counter_initialized()) {
      if (in_render_pass_) {
        VkRenderPass saved_render_pass = current_render_pass_;
        const VulkanRenderTargetCache::Framebuffer* saved_framebuffer =
            current_framebuffer_;
        {
          PassEndReasonScope pass_end_reason_scope(*this,
                                                   PassEndReason::kQuery);
          EndRenderPass();
        }
        zpd_host_query_pool_->ClearFSICounter(deferred_command_buffer_,
                                              zpd_active_query_index_);

        bool saved_pending_begin = zpd_active_segment_.segment_pending_begin;
        zpd_active_segment_.segment_pending_begin = false;
        SubmitBarriersAndEnterRenderTargetCacheRenderPass(saved_render_pass,
                                                          saved_framebuffer);
        zpd_active_segment_.segment_pending_begin = saved_pending_begin;
        fsi_counter_cleared = in_render_pass_;
      } else {
        zpd_host_query_pool_->ClearFSICounter(deferred_command_buffer_,
                                              zpd_active_query_index_);
        fsi_counter_cleared = true;
      }
    }
    if (!fsi_counter_cleared) {
      zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                              zpd_active_query_generation_);
      zpd_active_query_index_ = UINT32_MAX;
      zpd_active_query_generation_ = 0;
      zpd_active_query_is_fsi_ = false;
      zpd_fsi_counter_index_force_update_ = true;
      return QueryOpenResult::kFailed;
    }
    zpd_fsi_counter_index_force_update_ = true;
    return QueryOpenResult::kOpened;
  }

  zpd_host_query_pool_->BeginQuery(deferred_command_buffer_,
                                   zpd_active_query_index_);
  return QueryOpenResult::kOpened;
}

bool VulkanCommandProcessor::CloseZPDQuery(ReportHandle report_handle,
                                           uint64_t& out_submission) {
  if (!zpd_active_query_is_fsi_ && !in_render_pass_) {
    XELOGW("ZPD: Split segment requested outside render pass");
    return false;
  }

  if (zpd_active_query_is_fsi_) {
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, true);
  } else {
    zpd_host_query_pool_->EndQuery(deferred_command_buffer_,
                                   zpd_active_query_index_);
    zpd_host_query_pool_->QueueQueryResolve(zpd_active_query_index_, false);
  }

  PendingQueryResolve resolve;
  resolve.submission = GetCurrentSubmission();
  resolve.query_index = zpd_active_query_index_;
  resolve.query_generation = zpd_active_query_generation_;
  resolve.scale_area = GetZPDScaleArea();
  resolve.uses_fsi_counter = zpd_active_query_is_fsi_;
  resolve.report_handle = report_handle;
  zpd_resolves_in_flight_.push_back(resolve);

  out_submission = resolve.submission;

  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  bool closed_fsi_counter = zpd_active_query_is_fsi_;
  zpd_active_query_is_fsi_ = false;
  if (closed_fsi_counter) {
    zpd_fsi_counter_index_force_update_ = true;
  }
  return true;
}

bool VulkanCommandProcessor::DiscardZPDQuery() {
  if (zpd_active_query_is_fsi_) {
    // The slot counter may be dirty if draws ran between OpenZPDQuery and
    // here, but the next OpenZPDQuery clears it before any new shader adds.
    if (cvars::occlusion_query_log) {
      XELOGI("ZPD/Vulkan: Discarding FSI counter segment index={}",
             zpd_active_query_index_);
    }
    zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                            zpd_active_query_generation_);
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    zpd_fsi_counter_index_force_update_ = true;
    return true;
  }

  if (!in_render_pass_) {
    // vkCmdEndQuery is invalid outside a render pass for occlusion queries.
    // Defer the release until the submission containing the stale BeginQuery
    // completes on the GPU.
    XELOGW("ZPD: Discard segment requested outside render pass");
    zpd_deferred_releases_.push_back({GetCurrentSubmission(),
                                      zpd_active_query_index_,
                                      zpd_active_query_generation_});
    zpd_active_query_index_ = UINT32_MAX;
    zpd_active_query_generation_ = 0;
    zpd_active_query_is_fsi_ = false;
    return true;
  }

  // Inside a render pass, EndQuery must be issued before releasing the slot.
  zpd_host_query_pool_->EndQuery(deferred_command_buffer_,
                                 zpd_active_query_index_);
  zpd_host_query_pool_->ReleaseQueryIndex(zpd_active_query_index_,
                                          zpd_active_query_generation_);
  zpd_active_query_index_ = UINT32_MAX;
  zpd_active_query_generation_ = 0;
  zpd_active_query_is_fsi_ = false;
  return true;
}

void VulkanCommandProcessor::PumpQueryResolves() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_host_query_pool_) {
    return;
  }

  uint64_t completed = GetCompletedSubmission();
  if (completed == 0) {
    return;
  }

  // Drain deferred releases first.
  while (!zpd_deferred_releases_.empty()) {
    auto& entry = zpd_deferred_releases_.front();
    if (entry.submission > completed) {
      break;
    }
    zpd_host_query_pool_->ReleaseQueryIndex(entry.query_index,
                                            entry.query_generation);
    zpd_deferred_releases_.pop_front();
  }

  // Invalidate CPU cache before reading results on non-coherent memory.
  if (!zpd_resolves_in_flight_.empty() &&
      zpd_resolves_in_flight_.front().submission <= completed) {
    zpd_host_query_pool_->InvalidateReadback();
  }

  while (!zpd_resolves_in_flight_.empty()) {
    if (zpd_resolves_in_flight_.front().submission > completed) {
      break;
    }
    PendingQueryResolve resolve = zpd_resolves_in_flight_.front();
    zpd_resolves_in_flight_.pop_front();

    if (zpd_host_query_pool_->GenerationMatches(resolve.query_index,
                                                resolve.query_generation)) {
      uint64_t raw_samples = zpd_host_query_pool_->GetQueryReadbackValue(
          resolve.query_index, resolve.uses_fsi_counter);
      if (cvars::occlusion_query_log) {
        XELOGI("ZPD/Vulkan: Resolved {} segment index={} samples={}",
               resolve.uses_fsi_counter ? "FSI" : "native", resolve.query_index,
               raw_samples);
      }
      zpd_host_query_pool_->ReleaseQueryIndex(resolve.query_index,
                                              resolve.query_generation);
      OnZPDQueryResolved(resolve.report_handle, raw_samples,
                         resolve.scale_area);
    }
  }
}

bool VulkanCommandProcessor::AwaitQueryResolve(ReportHandle report_handle,
                                               uint64_t wait_for_submission) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  PumpQueryResolves();

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return true;
  }
  if (it->second.pending_segments == 0 && it->second.ended) {
    return true;
  }
  if (wait_for_submission == 0) {
    return false;
  }

  // Ensure the submission is flushed.
  if (wait_for_submission >= GetCurrentSubmission()) {
    if (!submission_open_) {
      return false;
    }
    if (!CanEndSubmissionImmediately()) {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD/Async: Draining Vulkan async pipeline creation for strict "
            "retirement");
      }
      pipeline_cache_->AwaitPipelineCompletion();
    }
    {
      PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kQuery);
      EndRenderPass();
    }
    if (!EndSubmission(false)) {
      return false;
    }
  }

  if (wait_for_submission > GetCompletedSubmission()) {
    completion_timeline_.AwaitSubmissionAndUpdateCompleted(wait_for_submission);
  }

  PumpQueryResolves();

  it = logical_zpd_reports_.find(report_handle);
  return it == logical_zpd_reports_.end() ||
         (it->second.pending_segments == 0 && it->second.ended);
}

void VulkanCommandProcessor::InitializeTrace() {
  CommandProcessor::InitializeTrace();

  if (!BeginSubmission(true)) {
    return;
  }
  // TODO(Triang3l): Write the EDRAM.
  bool shared_memory_submitted =
      shared_memory_->InitializeTraceSubmitDownloads();
  if (!shared_memory_submitted) {
    return;
  }
  AwaitAllQueueOperationsCompletion();
  if (shared_memory_submitted) {
    shared_memory_->InitializeTraceCompleteDownloads();
  }
}

void VulkanCommandProcessor::LogRecentSubmissions(const char* context) {
  XELOGE(
      "VulkanCommandProcessor: recent submission history ({}) - oldest first, "
      "current in-progress last:",
      context);
  // Walk the ring starting from the oldest entry.
  for (size_t i = 0; i < kSubmissionHistorySize; ++i) {
    const SubmissionSummary& s =
        submission_history_[(submission_history_next_ + i) %
                            kSubmissionHistorySize];
    if (!s.submission_index) {
      continue;
    }
    XELOGE(
        "  sub {:>5} frame {:>5}: draws={} dispatches={} resolves={} "
        "VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
  if (submission_open_) {
    const SubmissionSummary& s = submission_in_progress_;
    XELOGE(
        "  sub {:>5} frame {:>5} (in-progress): draws={} dispatches={} "
        "resolves={} VS={:016X} PS={:016X} RP=0x{:08X}",
        s.submission_index, s.frame_index, s.draw_count, s.dispatch_count,
        s.resolve_count, s.last_vs_hash, s.last_ps_hash,
        uint32_t(s.last_render_pass_key));
  }
}

void VulkanCommandProcessor::CheckSubmissionCompletionAndDeviceLoss(
    uint64_t await_submission) {
  void* const await_caller = __builtin_return_address(0);
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  if (cvars::log_gpu_frame_time_breakdown) {
    const uint64_t completed_before = GetCompletedSubmission();
    const uint64_t t0 = FrameStatsNow();
    completion_timeline_.AwaitSubmissionAndUpdateCompleted(await_submission);
    const uint64_t t1 = FrameStatsNow();
    if (memexport_await_reason_ && memexport_log_left_) {
      --memexport_log_left_;
      XELOGI(
          "MemexportLog: await for {} at draw {} of frame {}: submission {} "
          "(completed {} before), {} readbacks pending, {:.2f} ms",
          memexport_await_reason_, frame_draw_index_, frame_current_,
          await_submission, completed_before,
          pending_memexport_readbacks_.size(), double(t1 - t0) * 1e-6);
    }
    if (await_submission) {
      vk_frame_sync_stats_.awaits++;
      vk_frame_sync_stats_.await_ns += t1 - t0;
      if (await_submission > completed_before) {
        // The await actually blocked; record how far behind the GPU was.
        vk_frame_sync_stats_.blocking_awaits++;
        const uintptr_t await_address =
            reinterpret_cast<uintptr_t>(await_caller);
        for (AwaitSite& site : await_sites) {
          if (!site.address || site.address == await_address) {
            site.address = await_address;
            ++site.count;
            site.ns += t1 - t0;
            break;
          }
        }
        vk_frame_sync_stats_.await_delta +=
            GetCurrentSubmission() - await_submission;
      }
    }
    const uint64_t completed = GetCompletedSubmission();
    bool timestamps_invalidated = false;
    while (!vk_submit_times_.empty() &&
           vk_submit_times_.front().submission <= completed) {
      const SubmitTimeRecord& record = vk_submit_times_.front();
      // Set when the submission's GPU time passes
      // vulkan_log_slow_submission_ms.
      uint64_t slow_submission_exec_ns = 0;
      const uint64_t latency = t1 - record.submit_ns;
      vk_frame_sync_stats_.sub_latency_ns += latency;
      vk_frame_sync_stats_.sub_latency_max_ns =
          std::max(vk_frame_sync_stats_.sub_latency_max_ns, latency);
      vk_frame_sync_stats_.sub_completions++;
      if (record.timestamp_slot != UINT32_MAX && frame_timestamp_mapping_) {
        if (!timestamps_invalidated) {
          // The readback memory type is not guaranteed to be host-coherent.
          const ui::vulkan::VulkanDevice* const vulkan_device =
              GetVulkanDevice();
          VkMappedMemoryRange invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          invalidate_range.memory = frame_timestamp_buffer_memory_;
          invalidate_range.offset = 0;
          invalidate_range.size = VK_WHOLE_SIZE;
          vulkan_device->functions().vkInvalidateMappedMemoryRanges(
              vulkan_device->device(), 1, &invalidate_range);
          timestamps_invalidated = true;
        }
        const uint64_t ts_top =
            frame_timestamp_mapping_[record.timestamp_slot * 2];
        const uint64_t ts_bottom =
            frame_timestamp_mapping_[record.timestamp_slot * 2 + 1];
        const double period_ns =
            double(GetVulkanDevice()->properties().timestampPeriod);
        if (ts_bottom > ts_top) {
          const uint64_t exec_ns = uint64_t((ts_bottom - ts_top) * period_ns);
          vk_frame_sync_stats_.gpu_exec_ns += exec_ns;
          vk_frame_sync_stats_.gpu_exec_max_ns =
              std::max(vk_frame_sync_stats_.gpu_exec_max_ns, exec_ns);
          vk_frame_sync_stats_.gpu_samples++;
          if (cvars::vulkan_log_slow_submission_ms > 0 &&
              exec_ns > uint64_t(cvars::vulkan_log_slow_submission_ms) *
                            1000000) {
            // Logged once its passes are read below.
            slow_submission_exec_ns = exec_ns;
          }
        }
        if (frame_timestamp_prev_end_ && ts_top > frame_timestamp_prev_end_) {
          vk_frame_sync_stats_.gpu_gap_ns +=
              uint64_t((ts_top - frame_timestamp_prev_end_) * period_ns);
        }
        frame_timestamp_prev_end_ = ts_bottom;
        if (record.resolve_pair_count && resolve_timestamp_mapping_) {
          VkMappedMemoryRange resolve_invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          resolve_invalidate_range.memory = resolve_timestamp_buffer_memory_;
          resolve_invalidate_range.offset = 0;
          resolve_invalidate_range.size = VK_WHOLE_SIZE;
          GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
              GetVulkanDevice()->device(), 1, &resolve_invalidate_range);
          for (uint32_t i = 0; i < record.resolve_pair_count; ++i) {
            const uint64_t* r =
                resolve_timestamp_mapping_ + (record.resolve_slot_base + i) *
                                                 kResolveTimestampsPerResolve;
            const uint64_t r0 = r[0];
            const uint64_t r_copy_end = r[1];
            const uint64_t r1 = r[2];
            if (r1 > r0) {
              const uint64_t resolve_ns = uint64_t((r1 - r0) * period_ns);
              vk_frame_sync_stats_.resolve_gpu_ns += resolve_ns;
              vk_frame_sync_stats_.resolve_gpu_max_ns = std::max(
                  vk_frame_sync_stats_.resolve_gpu_max_ns, resolve_ns);
              vk_frame_sync_stats_.resolve_gpu_samples++;
              ResolveBucketStat& bucket =
                  resolve_bucket_stats_[resolve_ts_keys_[record.resolve_slot_base +
                                                         i]];
              bucket.ns += resolve_ns;
              bucket.max_ns = std::max(bucket.max_ns, resolve_ns);
              ++bucket.count;
              if (r_copy_end >= r0 && r1 >= r_copy_end) {
                bucket.copy_ns += uint64_t((r_copy_end - r0) * period_ns);
                bucket.clear_ns += uint64_t((r1 - r_copy_end) * period_ns);
              }
            }
          }
        }
        // The slow submission's longest passes and work outside passes.
        struct SlowPart {
          uint32_t key;
          uint64_t ns;
          uint32_t draws;
        };
        SlowPart slow_parts[4] = {};
        uint64_t slow_pass_ns = 0;
        if (record.pass_pair_count && pass_timestamp_mapping_) {
          VkMappedMemoryRange pass_invalidate_range = {
              VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          pass_invalidate_range.memory = pass_timestamp_buffer_memory_;
          pass_invalidate_range.offset = 0;
          pass_invalidate_range.size = VK_WHOLE_SIZE;
          GetVulkanDevice()->functions().vkInvalidateMappedMemoryRanges(
              GetVulkanDevice()->device(), 1, &pass_invalidate_range);
          for (uint32_t i = 0; i < record.pass_pair_count; ++i) {
            const uint32_t pair = record.pass_slot_base + i;
            const uint64_t p0 = pass_timestamp_mapping_[pair * 2];
            const uint64_t p1 = pass_timestamp_mapping_[pair * 2 + 1];
            if (p1 > p0) {
              if (slow_submission_exec_ns) {
                const uint64_t part_ns = uint64_t((p1 - p0) * period_ns);
                slow_pass_ns += part_ns;
                SlowPart part = {pass_ts_keys_[pair], part_ns,
                                 pass_ts_draws_[pair]};
                for (SlowPart& slot : slow_parts) {
                  if (part.ns > slot.ns) {
                    std::swap(part, slot);
                  }
                }
              }
              auto& bucket = pass_bucket_stats_[pass_ts_keys_[pair]];
              bucket.ns += uint64_t((p1 - p0) * period_ns);
              bucket.passes++;
              bucket.draws += pass_ts_draws_[pair];
              bucket.max_scissor_w =
                  std::max(bucket.max_scissor_w, pass_ts_scissor_[pair] >> 16);
              bucket.max_scissor_h = std::max(bucket.max_scissor_h,
                                              pass_ts_scissor_[pair] & 0xFFFF);
              bucket.max_viewport_w = std::max(bucket.max_viewport_w,
                                               pass_ts_viewport_[pair] >> 16);
              bucket.max_viewport_h = std::max(
                  bucket.max_viewport_h, pass_ts_viewport_[pair] & 0xFFFF);
            }
          }
        }
        if (slow_submission_exec_ns) {
          const SubmissionWork& work = record.work;
          std::string parts;
          for (const SlowPart& part : slow_parts) {
            if (!part.ns) {
              continue;
            }
            if (part.key & kMiscTimestampKeyBit) {
              parts += fmt::format(" misc{:08X} {:.1f}ms", part.key,
                                   part.ns / 1e6);
            } else {
              parts += fmt::format(" {}{}x{} {:.1f}ms/{}d",
                                   (part.key & 0x80000000u) ? "xfer " : "",
                                   (part.key >> 16) & 0x7FFF,
                                   part.key & 0xFFFF, part.ns / 1e6,
                                   part.draws);
            }
          }
          XELOGI(
              "VkSlowSubmission: {} GPU {:.1f}ms | draws={} resolves={} "
              "texture_loads={} texels={:.2f}M uploads={}KB | timed "
              "{:.1f}ms, longest:{}",
              record.submission, slow_submission_exec_ns / 1e6, work.draws,
              work.resolves, work.texture_loads,
              work.texture_load_texels / 1e6, work.upload_bytes >> 10,
              slow_pass_ns / 1e6, parts);
        }
      }
      vk_submit_times_.pop_front();
    }
  } else {
    completion_timeline_.AwaitSubmissionAndUpdateCompleted(await_submission);
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  if (vulkan_device->IsLost()) {
    XELOGE(
        "VulkanCommandProcessor: device lost observed in submission-check - "
        "awaiting submission {}, current {}, completed {}, in-flight {}, "
        "frame {} (frame_open: {}, submission_open: {})",
        await_submission, GetCurrentSubmission(), GetCompletedSubmission(),
        command_buffers_submitted_.size(), frame_current_, frame_open_,
        submission_open_);
    LogRecentSubmissions("submission-check");
    device_lost_ = true;
    graphics_system_->OnHostGpuLossFromAnyThread(true);
    return;
  }

  const uint64_t completed_submission = GetCompletedSubmission();

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission =
        submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > completed_submission) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > completed_submission) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  ApplyCompletedMemexportReadbacks(completed_submission);

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(completed_submission);

  // Reclaim descriptor pools that the GPU has finished using.
  if (resolve_downscale_descriptor_pool_chain_) {
    resolve_downscale_descriptor_pool_chain_->Reclaim(completed_submission);
  }

  PumpQueryResolves();

  // Destroy objects scheduled for destruction.
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > completed_submission) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission =
      is_opening_frame
          ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight]
          : 0;
  CheckSubmissionCompletionAndDeviceLoss(await_submission);
  const uint64_t completed_submission = GetCompletedSubmission();
  if (device_lost_ || completed_submission < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) -
                       kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_;
         ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] >
          completed_submission) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;

    submission_in_progress_ = SubmissionSummary{};
    submission_in_progress_.submission_index = GetCurrentSubmission();
    submission_in_progress_.frame_index = frame_current_;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();
    deferred_setup_command_buffer_.Reset();
    if (shared_memory_) {
      shared_memory_->OnGpuSubmissionOpened();
    }

    // Reset cached state of the command buffer.
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    dynamic_primitive_topology_update_needed_ = true;
    dynamic_primitive_restart_enable_update_needed_ = true;
    dynamic_cull_mode_update_needed_ = true;
    dynamic_front_face_update_needed_ = true;
    dynamic_depth_test_enable_update_needed_ = true;
    dynamic_depth_write_enable_update_needed_ = true;
    dynamic_depth_compare_op_update_needed_ = true;
    dynamic_stencil_test_enable_update_needed_ = true;
    dynamic_stencil_op_front_update_needed_ = true;
    dynamic_stencil_op_back_update_needed_ = true;
    dynamic_depth_clamp_enable_update_needed_ = true;
    dynamic_polygon_mode_update_needed_ = true;
    dynamic_color_blend_enable_update_needed_ = true;
    dynamic_color_blend_equation_update_needed_ = true;
    dynamic_color_write_mask_update_needed_ = true;
    dynamic_shading_rate_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    // Fresh deferred command buffer: abandon any open pass-timestamp pair
    // without emitting an end (it would land in the new, unrelated buffer).
    pass_ts_open_pair_ = UINT32_MAX;
    current_guest_graphics_pipeline_ = nullptr;
    current_guest_graphics_pipeline_handle_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    current_index_buffer_ = VK_NULL_HANDLE;

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Conservatively re-derive all cached sampler parameters in the new
    // frame (cheap - one derivation pass per stage; the handles still go
    // through their per-submission UseSampler revalidation independently).
    current_samplers_fetch_up_to_date_vertex_ = 0;
    current_samplers_fetch_up_to_date_pixel_ = 0;

    // Log guest ZPD report stats every 100 frames.
    if (GetZPDMode() != ZPDMode::kFake && cvars::occlusion_query_log &&
        zpd_host_query_pool_ && zpd_host_query_pool_->capacity() &&
        frame_current_ - zpd_stats_.last_log_frame >= 100) {
      XELOGI(
          "Occlusion Query Stats (last 100 frames): "
          "LogicalBegun={}, LogicalEnded={}, SegBegun={}, SegEnded={}, "
          "PoolExhausted={}, Failed={}, Wraps={}, SameSlotReuse={}",
          zpd_stats_.logical_begun, zpd_stats_.logical_ended,
          zpd_stats_.segments_begun, zpd_stats_.segments_ended,
          zpd_stats_.pool_exhausted, zpd_stats_.failed,
          zpd_stats_.counter_wraps, zpd_stats_.same_slot_reuse);

      zpd_stats_.Reset(frame_current_);
    }

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0,
                sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0,
                sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
            shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1)
        << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;
    // MANDATORY: the transient descriptor pool resets at the frame boundary, so
    // every cached texture set written in the previous frame is now invalid.
    // Invalidate in lockstep with the values bitmask reset and the
    // current_graphics_descriptor_sets_ memset above, otherwise a first-draw
    // whose content hash equals the previous frame's could skip the rewrite and
    // sample recycled transient-pool memory (the #1 corruption path).
    current_texture_descriptor_set_hash_valid_vertex_ = false;
    current_texture_descriptor_set_hash_valid_pixel_ = false;
    // Same MANDATORY invalidation for the constants value cache: the cached
    // transient set and the page buffers it references are reclaimable at the
    // frame boundary (the current_graphics_descriptor_sets_ memset above already
    // dropped the cached handle), so any stale per-binding VkBuffer must not let
    // a first draw skip the rewrite.
    constants_descriptor_set_valid_ = false;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(
                                             used_transient_descriptor.layout)]
          .push_back(used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(
          used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(
          used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it =
            texture_transient_descriptor_sets_free_
                .emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(used_transient_descriptor_set.layout),
                    std::forward_as_tuple())
                .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();

    render_target_cache_->EdramTraceBeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_ ||
         !pipeline_cache_->IsCreatingPipelines();
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  PassEndReasonScope pass_end_reason_scope(*this, PassEndReason::kSubmission);
  ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr,
                                &semaphore) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex =
          vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        XELOGE("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType =
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        XELOGE("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    EndRenderPass();

    // memexport_readback_deferred: the host reads this submission's exports
    // once it completes.
    if (memexport_readbacks_in_submission_) {
      memexport_readbacks_in_submission_ = false;
      PushBufferMemoryBarrier(
          shared_memory_->buffer(), 0, VK_WHOLE_SIZE,
          guest_shader_pipeline_stages_ | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_HOST_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, false);
      SubmitBarriers(true);
    }

    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount =
          uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty()
              ? sparse_buffer_bind_infos_temp_.data()
              : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(
                vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result = dfn.vkQueueBindSparse(
            queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        XELOGE("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(
          sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }
    // Can't cross command buffer boundaries. Close the active segment first.
    CloseQuerySegment();

    SubmitBarriers(true);

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      XELOGE("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer,
                                 &command_buffer_begin_info) != VK_SUCCESS) {
      XELOGE("Failed to begin a Vulkan command buffer");
      return false;
    }
    uint32_t fs_timestamp_slot = UINT32_MAX;
    if (frame_timestamp_mapping_) {
      fs_timestamp_slot =
          uint32_t(GetCurrentSubmission() % kFrameTimestampSlots);
      dfn.vkCmdResetQueryPool(command_buffer.buffer, frame_timestamp_pool_,
                              fs_timestamp_slot * 2, 2);
      dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              frame_timestamp_pool_, fs_timestamp_slot * 2);
      if (resolve_timestamp_mapping_) {
        // Reset this submission's resolve-pair range before the deferred
        // buffer (which contains the writes) replays.
        dfn.vkCmdResetQueryPool(
            command_buffer.buffer, resolve_timestamp_pool_,
            uint32_t(GetCurrentSubmission() % kResolveTimestampRingSubmissions) *
                kResolveTimestampPairsPerSubmission *
                kResolveTimestampsPerResolve,
            kResolveTimestampPairsPerSubmission * kResolveTimestampsPerResolve);
      }
      if (pass_timestamp_mapping_) {
        dfn.vkCmdResetQueryPool(
            command_buffer.buffer, pass_timestamp_pool_,
            uint32_t(GetCurrentSubmission() % kPassTimestampRingSubmissions) *
                kPassTimestampPairsPerSubmission * 2,
            kPassTimestampPairsPerSubmission * 2);
      }
    }
    // Submission boundary for asynchronously created pipelines: by default
    // this waits until every pipeline referenced by the recorded deferred
    // binds has finished creation, so no draw is lost. With
    // vulkan_async_skip_draws (or during the startup storage preload) it
    // doesn't block, and deferred binds that still resolve to VK_NULL_HANDLE
    // at replay drop their draws. Also persists the driver pipeline cache
    // (throttled) and flushes the shader/pipeline storage files.
    pipeline_cache_->EndSubmission();
    if (!deferred_setup_command_buffer_.empty()) {
      // VkMiscTime for the setup command buffer (hoisted shared memory
      // uploads), in the pass ring reset above. Skipped if a region is still
      // open, as it would share its slot.
      uint32_t setup_ts_pair = UINT32_MAX;
      if (pass_timestamp_mapping_ && fs_timestamp_slot != UINT32_MAX &&
          pass_ts_open_pair_ == UINT32_MAX) {
        if (pass_ts_submission_ != GetCurrentSubmission()) {
          pass_ts_submission_ = GetCurrentSubmission();
          pass_ts_count_ = 0;
        }
        if (pass_ts_count_ < kPassTimestampPairsPerSubmission) {
          setup_ts_pair =
              uint32_t(pass_ts_submission_ % kPassTimestampRingSubmissions) *
                  kPassTimestampPairsPerSubmission +
              pass_ts_count_;
          pass_ts_keys_[setup_ts_pair] =
              MakeMiscTimestampKey(MiscTimestampKind::kSetupCommands, 0);
          pass_ts_draws_[setup_ts_pair] = 0;
          pass_ts_scissor_[setup_ts_pair] = 0;
          pass_ts_viewport_[setup_ts_pair] = 0;
          dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                  pass_timestamp_pool_, setup_ts_pair * 2);
        } else {
          ++pass_ts_dropped_;
        }
      }
      deferred_setup_command_buffer_.Execute(command_buffer.buffer);
      if (setup_ts_pair != UINT32_MAX) {
        dfn.vkCmdWriteTimestamp(command_buffer.buffer,
                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                pass_timestamp_pool_, setup_ts_pair * 2 + 1);
        ++pass_ts_count_;
      }
      VkMemoryBarrier setup_barrier;
      setup_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      setup_barrier.pNext = nullptr;
      setup_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      setup_barrier.dstAccessMask =
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      dfn.vkCmdPipelineBarrier(
          command_buffer.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &setup_barrier, 0, nullptr,
          0, nullptr);
    }
    const uint64_t replay_begin_ns = FrameStatsBegin();
    deferred_command_buffer_.Execute(command_buffer.buffer);

    // Record ZPD resolves before submitting.
    if (zpd_host_query_pool_) {
      zpd_host_query_pool_->RecordResolveBatch(command_buffer.buffer);
    }

    if (fs_timestamp_slot != UINT32_MAX) {
      dfn.vkCmdWriteTimestamp(
          command_buffer.buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          frame_timestamp_pool_, fs_timestamp_slot * 2 + 1);
      dfn.vkCmdCopyQueryPoolResults(
          command_buffer.buffer, frame_timestamp_pool_, fs_timestamp_slot * 2,
          2, frame_timestamp_buffer_,
          fs_timestamp_slot * 2 * sizeof(uint64_t), sizeof(uint64_t),
          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      if (resolve_timestamp_mapping_ &&
          resolve_ts_submission_ == GetCurrentSubmission() &&
          resolve_ts_count_) {
        // Copy only the pairs actually written this submission - reset-but-
        // unwritten queries under WAIT_BIT would hang the GPU.
        const uint32_t resolve_ts_base =
            uint32_t(GetCurrentSubmission() % kResolveTimestampRingSubmissions) *
            kResolveTimestampPairsPerSubmission;
        dfn.vkCmdCopyQueryPoolResults(
            command_buffer.buffer, resolve_timestamp_pool_,
            resolve_ts_base * kResolveTimestampsPerResolve,
            resolve_ts_count_ * kResolveTimestampsPerResolve,
            resolve_timestamp_buffer_,
            resolve_ts_base * kResolveTimestampsPerResolve * sizeof(uint64_t),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      if (pass_timestamp_mapping_ &&
          pass_ts_submission_ == GetCurrentSubmission() && pass_ts_count_) {
        // Only fully-closed pairs are counted in pass_ts_count_; a pass still
        // open across the split keeps its begin query but is not copied here.
        const uint32_t pass_ts_base =
            uint32_t(GetCurrentSubmission() % kPassTimestampRingSubmissions) *
            kPassTimestampPairsPerSubmission;
        dfn.vkCmdCopyQueryPoolResults(
            command_buffer.buffer, pass_timestamp_pool_, pass_ts_base * 2,
            pass_ts_count_ * 2, pass_timestamp_buffer_,
            pass_ts_base * 2 * sizeof(uint64_t), sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
      }
      VkMemoryBarrier fs_timestamp_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      fs_timestamp_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      fs_timestamp_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      dfn.vkCmdPipelineBarrier(command_buffer.buffer,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                               &fs_timestamp_barrier, 0, nullptr, 0, nullptr);
    }

    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      XELOGE("Failed to end a Vulkan command buffer");
      return false;
    }

    const uint64_t submission_index = GetCurrentSubmission();

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount =
          uint32_t(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask =
          current_submission_wait_stage_masks_.data();
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.buffer;
    const VkResult submit_result = completion_timeline_.AcquireFenceAndSubmit(
        vulkan_device->queue_family_graphics_compute(), 0, 1, &submit_info);
    if (replay_begin_ns) {
      vk_frame_sync_stats_.replay_ns += FrameStatsNow() - replay_begin_ns;
    }
    if (submit_result != VK_SUCCESS) {
      XELOGE(
          "VulkanCommandProcessor: Failed to submit a Vulkan command buffer - "
          "VkResult: {} (0x{:08X}), submission: {} (completed: {}, in-flight: "
          "{}), frame: {} (frame_open: {}, is_closing_frame: {}), "
          "wait_semaphores: {}, draw_resolution_scale: {}x{}",
          static_cast<int32_t>(submit_result),
          static_cast<uint32_t>(submit_result), GetCurrentSubmission(),
          GetCompletedSubmission(), command_buffers_submitted_.size(),
          frame_current_, frame_open_, is_closing_frame,
          submit_info.waitSemaphoreCount,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_x()
                               : 0,
          render_target_cache_ ? render_target_cache_->draw_resolution_scale_y()
                               : 0);
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        XELOGE(
            "VK_ERROR_DEVICE_LOST - GPU crashed or hung. This may be caused by "
            "an invalid shader, out-of-bounds memory access, or driver bug.");
      }
      LogRecentSubmissions("submit-failure");
      if (vulkan_device->IsLost() && !device_lost_) {
        device_lost_ = true;
        graphics_system_->OnHostGpuLossFromAnyThread(true);
      }
      return false;
    }
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_index,
                                                     semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_index, command_buffer);
    command_buffers_writable_.pop_back();

    submission_history_[submission_history_next_] = submission_in_progress_;
    submission_history_next_ =
        (submission_history_next_ + 1) % kSubmissionHistorySize;

    // Mark descriptor pool chains with submission index for reclaim tracking.
    if (resolve_downscale_descriptor_pool_chain_) {
      resolve_downscale_descriptor_pool_chain_->EndSubmission(submission_index);
    }

    submission_open_ = false;
    SubmissionWork submission_work = submission_work_;
    submission_work.draws = draws_since_submission_;
    submission_work_ = SubmissionWork();
    draws_since_submission_ = 0;
    vk_frame_sync_stats_.submissions++;
    if (cvars::log_gpu_frame_time_breakdown) {
      uint32_t resolve_base = 0;
      uint32_t resolve_pairs = 0;
      if (resolve_timestamp_mapping_ &&
          resolve_ts_submission_ == submission_index) {
        resolve_pairs = resolve_ts_count_;
        resolve_base =
            uint32_t(submission_index % kResolveTimestampRingSubmissions) *
            kResolveTimestampPairsPerSubmission;
      }
      uint32_t pass_base = 0;
      uint32_t pass_pairs = 0;
      if (pass_timestamp_mapping_ && pass_ts_submission_ == submission_index) {
        pass_pairs = pass_ts_count_;
        pass_base =
            uint32_t(submission_index % kPassTimestampRingSubmissions) *
            kPassTimestampPairsPerSubmission;
      }
      vk_submit_times_.push_back({submission_index, FrameStatsNow(),
                                  fs_timestamp_slot, resolve_base,
                                  resolve_pairs, pass_base, pass_pairs,
                                  submission_work});
      resolve_ts_count_ = 0;
      pass_ts_count_ = 0;
    }

    // Process any ZPD resolves that completed with this submission.
    // Block if strict mode has a pending result waiting on the guest sentinel.
    PumpQueryResolves();
    PumpPendingRetire();
  }

  if (is_closing_frame) {
    if (cvars::clear_memory_page_state) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }

    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] =
        GetCurrentSubmission() - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();
  // MANDATORY: all transient texture sets are destroyed here, so the value
  // cache for them is no longer valid.
  current_texture_descriptor_set_hash_valid_vertex_ = false;
  current_texture_descriptor_set_hash_valid_pixel_ = false;
  texture_set_frame_reuse_.clear();
  texture_set_frame_reuse_frame_ = UINT64_MAX;

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  // MANDATORY: the uniform-buffer transient allocator is Reset() below,
  // destroying every constants set, so the value cache (the cached set handle in
  // current_graphics_descriptor_sets_[kDescriptorSetConstants] and the
  // per-binding VkBuffer handles) is no longer valid.
  constants_descriptor_set_valid_ = false;
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_storage_image_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count =
      pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count =
      pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask &&
      !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >=
          pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset =
      pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset =
      pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         scratch_buffer_memory_);
}

void VulkanCommandProcessor::UpdateDynamicState(
    const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,
    reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t draw_resolution_scale_x, uint32_t draw_resolution_scale_y,
    bool depth_bias_in_pixel_shader,
    const VulkanPipelineCache::DynamicState& pipeline_dynamic_state) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  // merge_tiling_bands: a draw first executed in a later band, placed at its
  // rows of the render targets as tall as the screen.
  if (tiling_band_draw_y_offset_ && viewport_info.xy_extent[0] &&
      viewport_info.xy_extent[1]) {
    viewport.y += float(tiling_band_draw_y_offset_);
  }
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  // Scale the scissor to match the render target resolution scale
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  // merge_tiling_bands: a scissor covering the whole band covers the whole
  // screen (the other bands' draws of the same geometry aren't executed).
  if (tiling_band_draw_expand_scissor_) {
    if (scissor_rect.offset.y == 0 &&
        scissor_rect.extent.height >= tiling_band_rows_) {
      scissor_rect.extent.height = RenderTargetCache::kMergedTilingBandsHeight;
    } else {
      scissor_rect.offset.y += tiling_band_draw_y_offset_;
    }
  }
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    if (depth_bias_in_pixel_shader) {
      depth_bias_constant_factor = 0.0f;
      depth_bias_slope_factor = 0.0f;
    } else {
      draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal,
                                               depth_bias_slope_factor,
                                               depth_bias_constant_factor);
      depth_bias_constant_factor *=
          regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                  xenos::DepthRenderTargetFormat::kD24S8
              ? draw_util::kD3D10PolygonOffsetFactorUnorm24
              : draw_util::kD3D10PolygonOffsetFactorFloat24;
      // With non-square resolution scaling, make sure the worst-case impact is
      // reverted (slope only along the scaled axis), thus max. Per-draw scale
      // so native draws get the guest bias as is. More bias is better than less
      // bias; less bias means Z fighting with the background is more likely.
      depth_bias_slope_factor *=
          xenos::kPolygonOffsetScaleSubpixelUnit *
          float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    }
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_,
                    &depth_bias_constant_factor, sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_slope_factor_, &depth_bias_slope_factor,
                    sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(
          dynamic_depth_bias_constant_factor_, 0.0f,
          dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants,
                    sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
          stencil_ref_mask_front_reg =
              (pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back)
                  ? XE_GPU_REG_RB_STENCILREFMASK_BF
                  : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back =
          regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ !=
          stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ !=
          stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ !=
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ =
          stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ !=
          stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ ==
          dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(
            VK_STENCIL_FACE_FRONT_AND_BACK,
            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ ==
          dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(
            VK_STENCIL_FACE_FRONT_AND_BACK, dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_FRONT_BIT, dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(
              VK_STENCIL_FACE_BACK_BIT, dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // Extended dynamic state (VK_EXT_extended_dynamic_state / state2 / state3).
  // The pipeline cache resolved the exact Vulkan values these draw must use
  // (matching what a static pipeline would have baked) into
  // pipeline_dynamic_state; emit only the fields whose capability bit is set,
  // with dirty tracking so redundant setters aren't recorded.
  const VulkanPipelineCache::DynamicStateCapabilities& eds_caps =
      pipeline_cache_->dynamic_state_capabilities();
  if (eds_caps.extended_dynamic_state) {
    const VulkanPipelineCache::DynamicState& ds = pipeline_dynamic_state;

    // Primitive topology (EDS1 core).
    dynamic_primitive_topology_update_needed_ |=
        dynamic_primitive_topology_ != ds.primitive_topology;
    if (dynamic_primitive_topology_update_needed_) {
      dynamic_primitive_topology_ = ds.primitive_topology;
      deferred_command_buffer_.CmdVkSetPrimitiveTopology(
          dynamic_primitive_topology_);
      dynamic_primitive_topology_update_needed_ = false;
    }
    // Primitive restart enable (EDS2 core).
    dynamic_primitive_restart_enable_update_needed_ |=
        dynamic_primitive_restart_enable_ != ds.primitive_restart_enable;
    if (dynamic_primitive_restart_enable_update_needed_) {
      dynamic_primitive_restart_enable_ = ds.primitive_restart_enable;
      deferred_command_buffer_.CmdVkSetPrimitiveRestartEnable(
          dynamic_primitive_restart_enable_);
      dynamic_primitive_restart_enable_update_needed_ = false;
    }
    // Cull mode (EDS1 core).
    dynamic_cull_mode_update_needed_ |= dynamic_cull_mode_ != ds.cull_mode;
    if (dynamic_cull_mode_update_needed_) {
      dynamic_cull_mode_ = ds.cull_mode;
      deferred_command_buffer_.CmdVkSetCullMode(dynamic_cull_mode_);
      dynamic_cull_mode_update_needed_ = false;
    }
    // Front face (EDS1 core).
    dynamic_front_face_update_needed_ |= dynamic_front_face_ != ds.front_face;
    if (dynamic_front_face_update_needed_) {
      dynamic_front_face_ = ds.front_face;
      deferred_command_buffer_.CmdVkSetFrontFace(dynamic_front_face_);
      dynamic_front_face_update_needed_ = false;
    }

    // Depth / stencil dynamic state. The pipeline cache disables the whole
    // extended-dynamic-state capability on the fragment-shader-interlock path
    // (where these aren't fixed-function), so reaching here implies a render
    // pass with real depth / stencil state.
    {
      // Depth test enable (EDS1 core).
      dynamic_depth_test_enable_update_needed_ |=
          dynamic_depth_test_enable_ != ds.depth_test_enable;
      if (dynamic_depth_test_enable_update_needed_) {
        dynamic_depth_test_enable_ = ds.depth_test_enable;
        deferred_command_buffer_.CmdVkSetDepthTestEnable(
            dynamic_depth_test_enable_);
        dynamic_depth_test_enable_update_needed_ = false;
      }
      // Depth write enable (EDS1 core).
      dynamic_depth_write_enable_update_needed_ |=
          dynamic_depth_write_enable_ != ds.depth_write_enable;
      if (dynamic_depth_write_enable_update_needed_) {
        dynamic_depth_write_enable_ = ds.depth_write_enable;
        deferred_command_buffer_.CmdVkSetDepthWriteEnable(
            dynamic_depth_write_enable_);
        dynamic_depth_write_enable_update_needed_ = false;
      }
      // Depth compare op (EDS1 core).
      dynamic_depth_compare_op_update_needed_ |=
          dynamic_depth_compare_op_ != ds.depth_compare_op;
      if (dynamic_depth_compare_op_update_needed_) {
        dynamic_depth_compare_op_ = ds.depth_compare_op;
        deferred_command_buffer_.CmdVkSetDepthCompareOp(
            dynamic_depth_compare_op_);
        dynamic_depth_compare_op_update_needed_ = false;
      }
      // Stencil test enable (EDS1 core).
      dynamic_stencil_test_enable_update_needed_ |=
          dynamic_stencil_test_enable_ != ds.stencil_test_enable;
      if (dynamic_stencil_test_enable_update_needed_) {
        dynamic_stencil_test_enable_ = ds.stencil_test_enable;
        deferred_command_buffer_.CmdVkSetStencilTestEnable(
            dynamic_stencil_test_enable_);
        dynamic_stencil_test_enable_update_needed_ = false;
      }
      // Stencil ops (EDS1 core), tracked per face. memcmp because
      // VkStencilOpState bundles the (dynamic) compare/write masks and
      // reference, which are set separately and must be ignored here.
      auto stencil_ops_equal = [](const VkStencilOpState& a,
                                  const VkStencilOpState& b) {
        return a.failOp == b.failOp && a.passOp == b.passOp &&
               a.depthFailOp == b.depthFailOp && a.compareOp == b.compareOp;
      };
      dynamic_stencil_op_front_update_needed_ |=
          !stencil_ops_equal(dynamic_stencil_op_front_, ds.stencil_front);
      if (dynamic_stencil_op_front_update_needed_) {
        dynamic_stencil_op_front_ = ds.stencil_front;
        deferred_command_buffer_.CmdVkSetStencilOp(
            VK_STENCIL_FACE_FRONT_BIT, ds.stencil_front.failOp,
            ds.stencil_front.passOp, ds.stencil_front.depthFailOp,
            ds.stencil_front.compareOp);
        dynamic_stencil_op_front_update_needed_ = false;
      }
      dynamic_stencil_op_back_update_needed_ |=
          !stencil_ops_equal(dynamic_stencil_op_back_, ds.stencil_back);
      if (dynamic_stencil_op_back_update_needed_) {
        dynamic_stencil_op_back_ = ds.stencil_back;
        deferred_command_buffer_.CmdVkSetStencilOp(
            VK_STENCIL_FACE_BACK_BIT, ds.stencil_back.failOp,
            ds.stencil_back.passOp, ds.stencil_back.depthFailOp,
            ds.stencil_back.compareOp);
        dynamic_stencil_op_back_update_needed_ = false;
      }
    }

    // EDS3 sub-features.
    if (eds_caps.depth_clamp_enable) {
      dynamic_depth_clamp_enable_update_needed_ |=
          dynamic_depth_clamp_enable_ != ds.depth_clamp_enable;
      if (dynamic_depth_clamp_enable_update_needed_) {
        dynamic_depth_clamp_enable_ = ds.depth_clamp_enable;
        deferred_command_buffer_.CmdVkSetDepthClampEnableEXT(
            dynamic_depth_clamp_enable_);
        dynamic_depth_clamp_enable_update_needed_ = false;
      }
    }
    if (eds_caps.polygon_mode) {
      dynamic_polygon_mode_update_needed_ |=
          dynamic_polygon_mode_ != ds.polygon_mode;
      if (dynamic_polygon_mode_update_needed_) {
        dynamic_polygon_mode_ = ds.polygon_mode;
        deferred_command_buffer_.CmdVkSetPolygonModeEXT(dynamic_polygon_mode_);
        dynamic_polygon_mode_update_needed_ = false;
      }
    }
    {
      // Per-render-target blend state. Emit only for the attachments present in
      // the render pass (ds.color_rts_used), as a single contiguous range
      // covering [0, highest used] so the array is dense for the setter.
      uint32_t color_rts_used = ds.color_rts_used;
      uint32_t attachment_count =
          color_rts_used ? (32 - xe::lzcnt(color_rts_used)) : 0;
      if (attachment_count) {
        if (eds_caps.color_blend_enable) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (dynamic_color_blend_enable_[i] != ds.color_blend_enable[i]) {
              dynamic_color_blend_enable_[i] = ds.color_blend_enable[i];
              changed = true;
            }
          }
          dynamic_color_blend_enable_update_needed_ |= changed;
          if (dynamic_color_blend_enable_update_needed_) {
            deferred_command_buffer_.CmdVkSetColorBlendEnableEXT(
                0, attachment_count, dynamic_color_blend_enable_);
            dynamic_color_blend_enable_update_needed_ = false;
          }
        }
        if (eds_caps.color_blend_equation) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (std::memcmp(&dynamic_color_blend_equation_[i],
                            &ds.color_blend_equation[i],
                            sizeof(VkColorBlendEquationEXT)) != 0) {
              dynamic_color_blend_equation_[i] = ds.color_blend_equation[i];
              changed = true;
            }
          }
          dynamic_color_blend_equation_update_needed_ |= changed;
          if (dynamic_color_blend_equation_update_needed_) {
            deferred_command_buffer_.CmdVkSetColorBlendEquationEXT(
                0, attachment_count, dynamic_color_blend_equation_);
            dynamic_color_blend_equation_update_needed_ = false;
          }
        }
        if (eds_caps.color_write_mask) {
          bool changed = false;
          for (uint32_t i = 0; i < attachment_count; ++i) {
            if (dynamic_color_write_mask_[i] != ds.color_write_mask[i]) {
              dynamic_color_write_mask_[i] = ds.color_write_mask[i];
              changed = true;
            }
          }
          dynamic_color_write_mask_update_needed_ |= changed;
          if (dynamic_color_write_mask_update_needed_) {
            deferred_command_buffer_.CmdVkSetColorWriteMaskEXT(
                0, attachment_count, dynamic_color_write_mask_);
            dynamic_color_write_mask_update_needed_ = false;
          }
        }
      }
    }
  }

  // The shading rate (vulkan_shading_rate) - dynamic in every guest pipeline
  // when the device has the pipeline shading rate.
  if (GetVulkanDevice()->properties().pipelineFragmentShadingRate) {
    dynamic_shading_rate_update_needed_ |=
        dynamic_shading_rate_ != draw_shading_rate_;
    if (dynamic_shading_rate_update_needed_) {
      dynamic_shading_rate_ = draw_shading_rate_;
      deferred_command_buffer_.CmdVkSetFragmentShadingRateKHR(
          1 + (dynamic_shading_rate_ & 1), 1 + ((dynamic_shading_rate_ >> 1) & 1));
      dynamic_shading_rate_update_needed_ = false;
    }
  }
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask,
    const draw_util::HostDepthPolygonOffset* host_depth_polygon_offset) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() ==
      RenderTargetCache::Path::kPixelShaderInterlock;
  // Resolution scale of this draw.
  // 1x1 with draw_resolution_scale_threshold (FSI only).
  uint32_t draw_resolution_scale_x = render_target_cache_->GetDrawScaleX();
  uint32_t draw_resolution_scale_y = render_target_cache_->GetDrawScaleY();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  // The color info registers are also read by the non-FSI gamma flag and color
  // exponent bias below.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    color_infos[i] = regs.Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[i]);
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::
          kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count - the host's (sample masks of a 4x surface stored at 2x
  // have 2 bits).
  flags |= uint32_t(render_target_cache_->GetHostMsaaSamples(
               rb_surface_info.msaa_samples))
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                        : xenos::CompareFunction::kAlways;
  // alpha_to_coverage_as_alpha_test: alpha to coverage without an alpha test
  // of the draw's own becomes the test alpha >= 0.5.
  if (cvars::alpha_to_coverage_as_alpha_test &&
      rb_colorcontrol.alpha_to_mask_enable &&
      alpha_test_function == xenos::CompareFunction::kAlways) {
    alpha_test_function = xenos::CompareFunction::kGreaterEqual;
  }
  flags |= uint32_t(alpha_test_function)
           << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing. When gamma is stored as unorm16, the host render target
  // holds linear values (blended in linear space) and the linear -> gamma
  // encode happens on the EDRAM store, so the pixel shader must not pre-encode.
  if (!edram_fragment_shader_interlock &&
      !render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format ==
          xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock) {
    // Fragment shader interlock (EDRAM ROP) flag bits and EDRAM constants. The
    // ZPD occlusion counter slot is selected here from Vulkan-specific query
    // state and passed into the shared helper.
    uint32_t zpd_fsi_counter_index = UINT32_MAX;
    if (zpd_active_query_index_ != UINT32_MAX && zpd_active_query_is_fsi_ &&
        zpd_host_query_pool_->fsi_counter_initialized()) {
      zpd_fsi_counter_index = zpd_active_query_index_;
    }
    dirty |= zpd_fsi_counter_index_force_update_;
    zpd_fsi_counter_index_force_update_ = false;
    WriteFragmentShaderInterlockSystemConstants(
        system_constants_, flags, dirty, regs, primitive_polygonal,
        normalized_depth_control, normalized_color_mask,
        draw_resolution_scale_x, draw_resolution_scale_y,
        zpd_fsi_counter_index);
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags &
      (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
       SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |= system_constants_.vertex_index_load_address !=
             primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address =
        primitive_processing_result.guest_index_base;
  }

  // Guest-side vertex index count, used for bounds-safe shared-memory loads in
  // VS expansion (kPointListAsTriangleStrip / kRectangleListAsTriangleStrip)
  // and in the full 32-bit index DMA load path. Out-of-bounds lanes read 0
  // instead of random memory, which otherwise produces scattered/skewed
  // geometry when expansion fans out past the guest draw count.
  const bool is_vs_expansion_draw =
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kPointListAsTriangleStrip ||
      primitive_processing_result.host_vertex_shader_type ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
  const uint32_t vertex_index_count =
      is_vs_expansion_draw ? primitive_processing_result.guest_draw_vertex_count
                           : primitive_processing_result.host_draw_vertex_count;
  dirty |= system_constants_.vertex_index_count != vertex_index_count;
  system_constants_.vertex_index_count = vertex_index_count;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian !=
           primitive_processing_result.host_shader_index_endian;
  system_constants_.vertex_index_endian =
      primitive_processing_result.host_shader_index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // User clip planes, for vertex and domain shaders.
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  if (!pa_cl_clip_cntl.clip_disable && pa_cl_clip_cntl.ucp_ena) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (xe::bit_scan_forward(user_clip_planes_remaining,
                                &user_clip_plane_index)) {
      user_clip_planes_remaining =
          xe::clear_lowest_bit(user_clip_planes_remaining);
      // Validate plane index is within bounds (0-5).
      assert(user_clip_plane_index < 6);
      if (user_clip_plane_index >= 6) {
        continue;
      }
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs,
                      4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs,
                    4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  } else {
    constexpr float kZeroPlanes[6][4] = {};
    if (std::memcmp(system_constants_.user_clip_planes, kZeroPlanes,
                    sizeof(system_constants_.user_clip_planes))) {
      dirty = true;
      std::memset(system_constants_.user_clip_planes, 0,
                  sizeof(system_constants_.user_clip_planes));
    }
  }

  // Tessellation constants. The factor range has 1.0 added per Xbox 360 docs.
  // fractional_even partitioning needs a minimum of 2.0.
  {
    float tess_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
    float tess_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
    dirty |= system_constants_.tessellation_factor_range[0] != tess_min;
    dirty |= system_constants_.tessellation_factor_range[1] != tess_max;
    system_constants_.tessellation_factor_range[0] = tess_min;
    system_constants_.tessellation_factor_range[1] = tess_max;
    uint32_t tess_vie = static_cast<uint32_t>(
        primitive_processing_result.host_shader_index_endian);
    uint32_t tess_vio = regs[XE_GPU_REG_VGT_INDX_OFFSET];
    uint32_t tess_vmin = regs[XE_GPU_REG_VGT_MIN_VTX_INDX];
    uint32_t tess_vmax = regs[XE_GPU_REG_VGT_MAX_VTX_INDX];
    dirty |= system_constants_.tessellation_vertex_index_endian != tess_vie;
    dirty |= system_constants_.tessellation_vertex_index_offset != tess_vio;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[0] != tess_vmin;
    dirty |=
        system_constants_.tessellation_vertex_index_min_max[1] != tess_vmax;
    system_constants_.tessellation_vertex_index_endian = tess_vie;
    system_constants_.tessellation_vertex_index_offset = tess_vio;
    system_constants_.tessellation_vertex_index_min_max[0] = tess_vmin;
    system_constants_.tessellation_vertex_index_min_max[1] = tess_vmax;
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min =
        float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max =
        float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x =
        float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y =
        float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min !=
             point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max !=
             point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] !=
             point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] !=
             point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint =
          system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs =
          texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs)
                                       << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1)
                                    << texture_signs_shift;
      dirty |=
          (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint =
          (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      uint32_t texture_integer_scale_bits =
          texture_cache_->GetActiveIntegerScaleBits(texture_index);
      dirty |= system_constants_.texture_integer_scale_bits[texture_index] !=
               texture_integer_scale_bits;
      system_constants_.texture_integer_scale_bits[texture_index] =
          texture_integer_scale_bits;
    }
  }

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint =
          system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle =
          texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle)
                                         << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1)
                                      << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) !=
               texture_swizzle_shifted;
      texture_swizzles_uint = (texture_swizzles_uint & ~texture_swizzle_mask) |
                              texture_swizzle_shifted;
    }
  }

  // Textures resolution scaled - which textures are from scaled resolve
  // operations.
  {
    uint32_t textures_resolved = 0;
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (xe::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      textures_resolved |=
          uint32_t(
              texture_cache_->IsActiveTextureResolutionScaled(texture_index))
          << texture_index;
    }
    dirty |= system_constants_.textures_resolved != textures_resolved;
    system_constants_.textures_resolved = textures_resolved;
  }

  // Alpha test (alpha_to_coverage_as_alpha_test: at 0.5 for the draws
  // whose alpha to coverage it turns into the test).
  const bool alpha_to_coverage_as_test =
      cvars::alpha_to_coverage_as_alpha_test &&
      rb_colorcontrol.alpha_to_mask_enable &&
      (!rb_colorcontrol.alpha_test_enable ||
       rb_colorcontrol.alpha_func == xenos::CompareFunction::kAlways);
  const float alpha_test_reference =
      alpha_to_coverage_as_test ? 0.5f : rb_alpha_ref;
  dirty |= system_constants_.alpha_test_reference != alpha_test_reference;
  system_constants_.alpha_test_reference = alpha_test_reference;

  // Alpha to coverage.
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable && !alpha_to_coverage_as_test
          ? (rb_colorcontrol.value >> 24) | (1 << 8)
          : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  // Color exponent bias.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() ==
            RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format ==
                 xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
  }

  if (!edram_fragment_shader_interlock && host_depth_polygon_offset) {
    draw_util::HostDepthPolygonOffset polygon_offset =
        *host_depth_polygon_offset;
    float scale_factor =
        float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    polygon_offset.front_scale *= scale_factor;
    polygon_offset.back_scale *= scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale !=
             polygon_offset.front_scale;
    system_constants_.edram_poly_offset_front_scale =
        polygon_offset.front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset !=
             polygon_offset.front_offset;
    system_constants_.edram_poly_offset_front_offset =
        polygon_offset.front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale !=
             polygon_offset.back_scale;
    system_constants_.edram_poly_offset_back_scale = polygon_offset.back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset !=
             polygon_offset.back_offset;
    system_constants_.edram_poly_offset_back_offset =
        polygon_offset.back_offset;
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader,
                                            bool vertex_bindings_ready,
                                            bool pixel_bindings_ready,
                                            bool interpreter_placeholder,
                                            bool placeholder_pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] !=
        float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] =
          float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  // The interpreter needs all 256 float constants unpacked; switching between
  // that and a shader's packed subset must re-upload.
  if (interpreter_placeholder != float_constants_vertex_are_full_) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
    float_constants_vertex_are_full_ = interpreter_placeholder;
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] !=
          float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] =
            float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0,
                sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  if ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) !=
      kAllConstantBuffersMask) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // vulkan_float_constants_full_range: all 256 float constants, what the
    // interpreter and dynamically indexed constants use.
    constexpr size_t kFloatConstantsFullRange = sizeof(float) * 4 * 256;
    const bool float_constants_full_range =
        use_dynamic_constants_ && cvars::vulkan_float_constants_full_range;
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, sizeof(SpirvShaderTranslator::SystemConstants),
          uniform_buffer_alignment, buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_,
                  sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      // The interpreter indexes all 256 float constants by raw index, so upload
      // the full contiguous register file rather than the packed subset.
      size_t float_constants_size =
          interpreter_placeholder
              ? sizeof(float) * 4 * 256
              : sizeof(float) * 4 *
                    std::max(float_constant_count_vertex, UINT32_C(1));
      const size_t float_constants_range =
          float_constants_full_range ? kFloatConstantsFullRange
                                     : float_constants_size;
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset, float_constants_range);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_range);
      if (interpreter_placeholder) {
        std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X],
                    sizeof(float) * 4 * 256);
      } else {
        for (uint32_t i = 0; i < 4; ++i) {
          uint64_t float_constant_map_entry =
              current_float_constant_map_vertex_[i];
          uint32_t float_constant_index;
          while (xe::bit_scan_forward(float_constant_map_entry,
                                      &float_constant_index)) {
            float_constant_map_entry &= ~(1ull << float_constant_index);
            std::memcpy(mapping,
                        &regs[XE_GPU_REG_SHADER_CONSTANT_000_X + (i << 8) +
                              (float_constant_index << 2)],
                        sizeof(float) * 4);
            mapping += sizeof(float) * 4;
          }
        }
      }
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      const size_t float_constants_range =
          float_constants_full_range ? kFloatConstantsFullRange
                                     : float_constants_size;
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, float_constants_size, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset, float_constants_range);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_range);
      for (uint32_t i = 0; i < 4; ++i) {
        uint64_t float_constant_map_entry =
            current_float_constant_map_pixel_[i];
        uint32_t float_constant_index;
        while (xe::bit_scan_forward(float_constant_map_entry,
                                    &float_constant_index)) {
          float_constant_map_entry &= ~(1ull << float_constant_index);
          std::memcpy(mapping,
                      &regs[XE_GPU_REG_SHADER_CONSTANT_256_X + (i << 8) +
                            (float_constant_index << 2)],
                      sizeof(float) * 4);
          mapping += sizeof(float) * 4;
        }
      }
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, kBoolLoopConstantsSize, uniform_buffer_alignment,
          buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                  kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info = current_constant_buffer_infos_
          [SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      constexpr size_t kFetchConstantsDecodedSize =
          sizeof(SpirvShaderTranslator::DecodedTextureFetchConstant) * 32;
      const bool fetch_constants_decoded =
          cvars::spirv_texture_fetch_constants_decoded;
      size_t fetch_constants_buffer_size =
          kFetchConstantsSize +
          (fetch_constants_decoded ? kFetchConstantsDecodedSize : 0);
      uint8_t* mapping = uniform_buffer_pool_->Request(
          frame_current_, fetch_constants_buffer_size,
          uniform_buffer_alignment, buffer_info.buffer, buffer_info.offset);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(fetch_constants_buffer_size);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                  kFetchConstantsSize);
      if (fetch_constants_decoded) {
        // Decode only the fetch constants written since the last time.
        uint32_t decode_remaining = fetch_constants_decode_needed_;
        fetch_constants_decode_needed_ = 0;
        uint32_t fetch_slot;
        while (xe::bit_scan_forward(decode_remaining, &fetch_slot)) {
          decode_remaining &= ~(UINT32_C(1) << fetch_slot);
          SpirvShaderTranslator::DecodeTextureFetchConstant(
              &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 6 * fetch_slot],
              fetch_constants_decoded_[fetch_slot]);
        }
        std::memcpy(mapping + kFetchConstantsSize,
                    fetch_constants_decoded_.data(),
                    kFetchConstantsDecodedSize);
      }
      current_constant_buffers_up_to_date_ |=
          UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch;
    }
  }

  // Textures and samplers. Only read a stage's binding lists when it was ready
  // at the draw's snapshot - a creation thread may still be populating them,
  // and the snapshot keeps the counts matching the collected sampler vectors.
  // Binding the reference is fine; only size()/iteration touch it.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex =
      vertex_bindings_ready ? uint32_t(samplers_vertex.size()) : 0;
  uint32_t texture_count_vertex =
      vertex_bindings_ready ? uint32_t(textures_vertex.size()) : 0;
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader && pixel_bindings_ready) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // Placeholder pipelines were built with a layout matching only what their
  // placeholder shaders access, so don't bind texture sets they lack (which
  // could otherwise happen if the real shaders finished translating mid-draw).
  // The no-op placeholder PS never samples; the interpreter VS never samples.
  // (Both flags stay false when vulkan_placeholder_pipelines is off.)
  if (placeholder_pixel_shader) {
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  if (interpreter_placeholder) {
    sampler_count_vertex = 0;
    texture_count_vertex = 0;
  }

  // The descriptor write and the set-hash below both rely on this; a shorter
  // vector would write fewer image infos than the count the set layout was
  // chosen by.
  assert_true(!sampler_count_vertex ||
              current_samplers_vertex_.size() == sampler_count_vertex);
  assert_true(!sampler_count_pixel ||
              current_samplers_pixel_.size() == sampler_count_pixel);

  // Value-cache the texture/sampler descriptor sets: only force a re-write (by
  // clearing the stage's values-up-to-date bit) when the resolved VkImageView /
  // VkSampler handles, the binding counts, or the layout changed since the last
  // successful write, OR an invalidation fired (the *_hash_valid_ flags are
  // reset on new frame / transient-pool reset / pipeline-layout change). When
  // nothing changed the bit stays set, the write-gate (write_*_textures below)
  // is false, and the still-valid cached set is left bound (no
  // vkUpdateDescriptorSets, no re-bind). The handle is hashed (never the fetch
  // constant index) so texture eviction/recreation, which re-resolves the
  // binding to a new handle before this point, always forces a re-write.
  //
  // Hash inputs, in this exact fixed order so an ordering/count change always
  // changes the hash: packed layout key (texture_count | sampler_count<<16 |
  // is_vertex<<31), then each resolved VkImageView in shader binding order,
  // then each resolved VkSampler in order.
  auto compute_texture_set_hash =
      [&scratch = texture_set_hash_scratch_](
          uint32_t texture_count, uint32_t sampler_count, bool is_vertex,
          const std::vector<VulkanShader::TextureBinding>* textures,
          VulkanTextureCache* texture_cache,
          const std::vector<
              std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>&
              samplers) -> uint64_t {
    // Gather the reuse inputs (in this exact fixed order) into a reused scratch
    // buffer and hash them with one XXH3 call - NEON-accelerated on arm64,
    // matching the rest of the codebase instead of the old byte-at-a-time
    // scalar FNV-1a. The result is only ever compared against a cached value
    // from this same function, so swapping the algorithm is transparent to the
    // reuse gate (different absolute values, identical same-inputs -> same-hash
    // behavior).
    scratch.clear();
    // Packed layout key (texture_count | sampler_count<<16 | is_vertex<<31).
    scratch.push_back(uint64_t(uint32_t(texture_count & 0xFFFF) |
                               ((sampler_count & 0x7FFF) << 16) |
                               (is_vertex ? (UINT32_C(1) << 31) : 0)));
    if (texture_count) {
      // Each resolved VkImageView in shader binding order (the handle, never
      // the fetch constant index - texture eviction/recreation re-resolves to a
      // new handle before this point, so a content change always forces a
      // re-write).
      for (const VulkanShader::TextureBinding& texture_binding : *textures) {
        scratch.push_back(uint64_t(reinterpret_cast<uintptr_t>(
            texture_cache->GetActiveBindingOrNullImageView(
                texture_binding.fetch_constant, texture_binding.dimension,
                bool(texture_binding.is_signed)))));
        // The written descriptor also carries the image layout, which changes
        // when a texture is promoted to a resolve destination mid-frame.
        scratch.push_back(uint64_t(texture_cache->GetActiveBindingImageLayout(
            texture_binding.fetch_constant, texture_binding.dimension,
            bool(texture_binding.is_signed))));
      }
    }
    if (sampler_count) {
      // Iterate the vector, like the descriptor write below, rather than
      // indexing it by a count sourced from the shader's binding list.
      for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
               sampler_pair : samplers) {
        scratch.push_back(
            uint64_t(reinterpret_cast<uintptr_t>(sampler_pair.second)));
      }
      // Handle values can be reused: a destroyed sampler's VkSampler value
      // may come back from vkCreateSampler for different parameters, which
      // would collide with the cached hash and leave a descriptor set
      // internally referencing the destroyed object bound. Folding the
      // destroy generation in forces a rewrite after any sampler
      // destruction.
      scratch.push_back(texture_cache->sampler_destroy_generation());
    }
    return XXH3_64bits(scratch.data(), scratch.size() * sizeof(scratch[0]));
  };
  // Computed below when the cache is enabled; reused in the successful-write
  // blocks to store the new cache entry.
  uint64_t texture_set_hash_vertex = 0;
  uint64_t texture_set_hash_pixel = 0;
  if (cvars::vulkan_cache_texture_descriptors) {
    if (cvars::vulkan_texture_descriptor_reuse_edge) {
      // A/B alternative: upstream edge's reuse gate (ported faithfully from
      // edge b5275a6cb UpdateBindings ~lines 6976-6998). A stage's cached set
      // is kept only when none of these changed since the last write: the
      // shader (and thus its binding list), a used texture's resolved host
      // image view (signalled by the texture cache's per-fetch-constant
      // bindings-changed mask), or any sampler in the stage's vector. The
      // content-hash is intentionally NOT computed here so edge mode's gate
      // cost equals edge's gate only. Used-texture masks are guarded by the
      // caller's readiness snapshot - an async draw may not have translated
      // the stage yet.
      uint32_t textures_changed = texture_cache_->texture_bindings_changed();
      uint32_t used_texture_mask_vertex =
          vertex_bindings_ready
              ? vertex_shader->GetUsedTextureMaskAfterTranslation()
              : 0;
      uint32_t used_texture_mask_pixel =
          (pixel_shader && pixel_bindings_ready)
              ? pixel_shader->GetUsedTextureMaskAfterTranslation()
              : 0;
      if ((current_graphics_descriptor_set_values_up_to_date_ &
           (UINT32_C(1)
            << SpirvShaderTranslator::kDescriptorSetTexturesVertex)) &&
          (current_textures_vertex_shader_ != vertex_shader ||
           (textures_changed & used_texture_mask_vertex) ||
           current_written_samplers_vertex_ != current_samplers_vertex_)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
      }
      if ((current_graphics_descriptor_set_values_up_to_date_ &
           (UINT32_C(1)
            << SpirvShaderTranslator::kDescriptorSetTexturesPixel)) &&
          (current_textures_pixel_shader_ != pixel_shader ||
           (textures_changed & used_texture_mask_pixel) ||
           current_written_samplers_pixel_ != current_samplers_pixel_)) {
        current_graphics_descriptor_set_values_up_to_date_ &= ~(
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
      }
      texture_cache_->ResetTextureBindingsChanged(used_texture_mask_vertex |
                                                  used_texture_mask_pixel);
    } else {
      // XenDroid's content-hash gate (default).
      // Vertex stage.
      if (texture_count_vertex || sampler_count_vertex) {
        texture_set_hash_vertex = compute_texture_set_hash(
            texture_count_vertex, sampler_count_vertex, true, &textures_vertex,
            texture_cache_.get(), current_samplers_vertex_);
        if (!current_texture_descriptor_set_hash_valid_vertex_ ||
            texture_set_hash_vertex !=
                current_texture_descriptor_set_hash_vertex_) {
          current_graphics_descriptor_set_values_up_to_date_ &= ~(
              UINT32_C(1)
              << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
        }
      }
      // Pixel stage.
      if (texture_count_pixel || sampler_count_pixel) {
        texture_set_hash_pixel = compute_texture_set_hash(
            texture_count_pixel, sampler_count_pixel, false, textures_pixel,
            texture_cache_.get(), current_samplers_pixel_);
        if (!current_texture_descriptor_set_hash_valid_pixel_ ||
            texture_set_hash_pixel !=
                current_texture_descriptor_set_hash_pixel_) {
          current_graphics_descriptor_set_values_up_to_date_ &= ~(
              UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
        }
      }
    }
  } else {
    // Opt-out: reproduce the original unconditional clear (re-write + re-bind
    // the texture/sampler descriptor sets every draw) for A/B debugging.
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~((UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex) |
          (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  }

  // Make sure new descriptor sets are bound to the command buffer.

  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Fill the texture and sampler write image infos.

  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex));
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel));
  // Texture sets are allocated from the pipeline layout, so its baked counts
  // must match the writes below. A mismatch means the pipeline still carries
  // the minimal layout from untranslated shaders, and writing into the
  // resulting zero-binding set crashes the driver. Fail the draw instead.
  if ((write_vertex_textures &&
       (current_guest_graphics_pipeline_layout_->texture_count_vertex() !=
            texture_count_vertex ||
        current_guest_graphics_pipeline_layout_->sampler_count_vertex() !=
            sampler_count_vertex)) ||
      (write_pixel_textures &&
       (current_guest_graphics_pipeline_layout_->texture_count_pixel() !=
            texture_count_pixel ||
        current_guest_graphics_pipeline_layout_->sampler_count_pixel() !=
            sampler_count_pixel))) {
    XELOGE(
        "UpdateBindings: pipeline layout counts (VS {}t/{}s, PS {}t/{}s) do "
        "not match the draw's binding counts (VS {}t/{}s, PS {}t/{}s) - "
        "skipping the draw",
        current_guest_graphics_pipeline_layout_->texture_count_vertex(),
        current_guest_graphics_pipeline_layout_->sampler_count_vertex(),
        current_guest_graphics_pipeline_layout_->texture_count_pixel(),
        current_guest_graphics_pipeline_layout_->sampler_count_pixel(),
        texture_count_vertex, sampler_count_vertex, texture_count_pixel,
        sampler_count_pixel);
    return false;
  }
  // vulkan_texture_descriptor_set_frame_reuse: a set written earlier in the
  // frame with the same content (the hash covers the layout, images and
  // samplers) is bound instead of writing a new one. Sets written in the frame
  // go back to the free list only once it completes; the map is dropped at
  // every frame and pool reset.
  if (cvars::vulkan_cache_texture_descriptors &&
      !cvars::vulkan_texture_descriptor_reuse_edge &&
      cvars::vulkan_texture_descriptor_set_frame_reuse &&
      (write_vertex_textures || write_pixel_textures)) {
    if (texture_set_frame_reuse_frame_ != frame_current_) {
      texture_set_frame_reuse_.clear();
      texture_set_frame_reuse_frame_ = frame_current_;
    }
    if (write_vertex_textures) {
      auto it = texture_set_frame_reuse_.find(texture_set_hash_vertex);
      if (it != texture_set_frame_reuse_.end()) {
        write_vertex_textures = false;
        current_graphics_descriptor_sets_
            [SpirvShaderTranslator::kDescriptorSetTexturesVertex] = it->second;
        current_graphics_descriptor_set_values_up_to_date_ |=
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
        current_textures_vertex_shader_ = vertex_shader;
        current_written_samplers_vertex_ = current_samplers_vertex_;
        current_texture_descriptor_set_hash_vertex_ = texture_set_hash_vertex;
        current_texture_descriptor_set_hash_valid_vertex_ = true;
        ++vk_frame_sync_stats_.texture_set_reuses;
      }
    }
    if (write_pixel_textures) {
      auto it = texture_set_frame_reuse_.find(texture_set_hash_pixel);
      if (it != texture_set_frame_reuse_.end()) {
        write_pixel_textures = false;
        current_graphics_descriptor_sets_
            [SpirvShaderTranslator::kDescriptorSetTexturesPixel] = it->second;
        current_graphics_descriptor_set_values_up_to_date_ |=
            UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
        current_textures_pixel_shader_ = pixel_shader;
        current_written_samplers_pixel_ = current_samplers_pixel_;
        current_texture_descriptor_set_hash_pixel_ = texture_set_hash_pixel;
        current_texture_descriptor_set_hash_valid_pixel_ = true;
        ++vk_frame_sync_stats_.texture_set_reuses;
      }
    }
  }
  // When a needed texture set is skipped (not re-written this draw), the cached
  // VkDescriptorSet must still be a real, allocated set - never the frame-open
  // VK_NULL_HANDLE memset value - or the bind loop below would bind a null
  // handle. A missed hash-validity reset would surface here as a fail-fast.
  assert_true(write_vertex_textures ||
              !(texture_count_vertex || sampler_count_vertex) ||
              current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetTexturesVertex] !=
                  VK_NULL_HANDLE);
  assert_true(write_pixel_textures ||
              !(texture_count_pixel || sampler_count_pixel) ||
              current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetTexturesPixel] !=
                  VK_NULL_HANDLE);
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(
      (write_vertex_textures ? texture_count_vertex + sampler_count_vertex
                             : 0) +
      (write_pixel_textures ? texture_count_pixel + sampler_count_pixel : 0));
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && texture_count_vertex) {
    for (const VulkanShader::TextureBinding& texture_binding :
         textures_vertex) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          texture_cache_->GetActiveBindingImageLayout(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
    }
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_vertex_textures && sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && texture_count_pixel) {
    for (const VulkanShader::TextureBinding& texture_binding :
         *textures_pixel) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.imageView =
          texture_cache_->GetActiveBindingOrNullImageView(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
      descriptor_image_info.imageLayout =
          texture_cache_->GetActiveBindingImageLayout(
              texture_binding.fetch_constant, texture_binding.dimension,
              bool(texture_binding.is_signed));
    }
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (write_pixel_textures && sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>&
             sampler_pair : current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info =
          descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet,
             SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  // Dynamic-constants descriptor buffer infos. These must outlive the single
  // deferred vkUpdateDescriptorSets below (each write_constants.pBufferInfo
  // points into this array), so they live at the same scope as
  // write_descriptor_sets rather than inside the rewrite branch.
  VkDescriptorBufferInfo constants_buffer_infos_dynamic
      [SpirvShaderTranslator::kConstantBufferCount];
  assert_not_zero(
      current_graphics_descriptor_set_values_up_to_date_ &
      (UINT32_C(1)
       << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  // The values bit was cleared at the upload site (above) whenever ANY of the 5
  // buffers' content changed, which is what forces the per-draw re-bind below.
  // With UNIFORM_BUFFER_DYNAMIC we DECOUPLE the descriptor WRITE from the bind:
  // the set is re-allocated and re-written only when a binding's backing
  // VkBuffer changed (upload-pool page rollover); otherwise the same set is
  // re-bound with fresh dynamic offsets. With plain UNIFORM_BUFFER (gate off)
  // the original unconditional alloc+write+bind runs.
  if (!(current_graphics_descriptor_set_values_up_to_date_ &
        (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    bool rewrite_needed = true;
    if (use_dynamic_constants_) {
      rewrite_needed = !constants_descriptor_set_valid_;
      for (uint32_t i = 0;
           !rewrite_needed && i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        // Re-write on page rollover (buffer changed) OR a range change. The
        // descriptor's range is baked at write time and only the dynamic offset
        // is re-bound per draw, so a stale (too small) range from a prior
        // shader with fewer float constants would silently clamp the current
        // shader's reads. See constants_descriptor_set_ranges_ in the header.
        if (current_constant_buffer_infos_[i].buffer !=
                constants_descriptor_set_buffers_[i] ||
            current_constant_buffer_infos_[i].range !=
                constants_descriptor_set_ranges_[i]) {
          rewrite_needed = true;
        }
      }
    }
    if (rewrite_needed) {
      const VkDescriptorType constants_descriptor_type =
          use_dynamic_constants_ ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                 : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      VkDescriptorSet constants_descriptor_set;
      if (!constants_transient_descriptors_free_.empty()) {
        constants_descriptor_set = constants_transient_descriptors_free_.back();
        constants_transient_descriptors_free_.pop_back();
      } else {
        VkDescriptorPoolSize constants_descriptor_count;
        // For UNIFORM_BUFFER_DYNAMIC this type is NOT present in the shared
        // allocator's pool sizes (kept at UNIFORM_BUFFER for the compute uniform
        // path), so LinkedTypeDescriptorSetAllocator::Allocate falls back to a
        // dedicated VkDescriptorPool per allocation. That is spec-legal, and
        // bounded: with the dynamic path we allocate only on page rollover
        // (0-2x/frame), not per draw, so the dedicated-pool churn is rare. Do
        // NOT retype the shared pool to fix this - the compute path needs the
        // UNIFORM_BUFFER pool size.
        constants_descriptor_count.type = constants_descriptor_type;
        constants_descriptor_count.descriptorCount =
            SpirvShaderTranslator::kConstantBufferCount;
        constants_descriptor_set =
            transient_descriptor_allocator_uniform_buffer_.Allocate(
                descriptor_set_layout_constants_, &constants_descriptor_count,
                1);
        if (constants_descriptor_set == VK_NULL_HANDLE) {
          return false;
        }
      }
      constants_transient_descriptors_used_.emplace_back(
          frame_current_, constants_descriptor_set);
      ++vk_frame_sync_stats_.constants_set_writes;
      // For UNIFORM_BUFFER_DYNAMIC the descriptor base offset MUST be 0: the
      // per-buffer slice offset is supplied as a dynamic offset at bind time, so
      // baking it into the descriptor as well would double-count it. The range
      // stays the per-buffer data size (<= 4 KiB, well below the
      // maxUniformBufferRange spec floor of 16384). With plain UNIFORM_BUFFER
      // the original {buffer, slice_offset, range} info is used directly.
      // (constants_buffer_infos_dynamic is declared at function scope above so
      // it outlives the deferred vkUpdateDescriptorSets.)
      if (use_dynamic_constants_) {
        for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
             ++i) {
          assert_true(current_constant_buffer_infos_[i].range <=
                      VkDeviceSize(GetVulkanDevice()
                                       ->properties()
                                       .maxUniformBufferRange));
          constants_buffer_infos_dynamic[i].buffer =
              current_constant_buffer_infos_[i].buffer;
          constants_buffer_infos_dynamic[i].offset = 0;
          constants_buffer_infos_dynamic[i].range =
              current_constant_buffer_infos_[i].range;
        }
      }
      // Consecutive bindings updated via a single VkWriteDescriptorSet must have
      // identical stage flags, but for the constants they vary.
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        VkWriteDescriptorSet& write_constants =
            write_descriptor_sets[write_descriptor_set_count++];
        write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write_constants.pNext = nullptr;
        write_constants.dstSet = constants_descriptor_set;
        write_constants.dstBinding = i;
        write_constants.dstArrayElement = 0;
        write_constants.descriptorCount = 1;
        write_constants.descriptorType = constants_descriptor_type;
        write_constants.pImageInfo = nullptr;
        write_constants.pBufferInfo =
            use_dynamic_constants_ ? &constants_buffer_infos_dynamic[i]
                                   : &current_constant_buffer_infos_[i];
        write_constants.pTexelBufferView = nullptr;
      }
      write_descriptor_set_bits |=
          UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
      current_graphics_descriptor_sets_
          [SpirvShaderTranslator::kDescriptorSetConstants] =
              constants_descriptor_set;
      if (use_dynamic_constants_) {
        for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
             ++i) {
          constants_descriptor_set_buffers_[i] =
              current_constant_buffer_infos_[i].buffer;
          constants_descriptor_set_ranges_[i] =
              current_constant_buffer_infos_[i].range;
        }
        constants_descriptor_set_valid_ = true;
      }
    } else {
      // Dynamic skip-rewrite path: the cached set's bindings still reference the
      // current page buffers, so only the dynamic offsets (bound below) change.
      // Re-set the values bit (do NOT push to the used list - the set was
      // already accounted for when written) so an unchanged subsequent draw
      // keeps the bit and skips even the re-bind, matching the texture-cache
      // no-rebind behavior. A content change will clear it again at the upload
      // site, re-triggering the rebind with fresh offsets.
      assert_true(current_graphics_descriptor_sets_
                      [SpirvShaderTranslator::kDescriptorSetConstants] !=
                  VK_NULL_HANDLE);
      write_descriptor_set_bits |=
          UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    }
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    if (cvars::vulkan_replay_stats) {
      CountTextureSetWrite(texture_set_hash_vertex);
    }
    if (texture_set_frame_reuse_frame_ == frame_current_ &&
        cvars::vulkan_texture_descriptor_set_frame_reuse &&
        !cvars::vulkan_texture_descriptor_reuse_edge) {
      texture_set_frame_reuse_.emplace(texture_set_hash_vertex,
                                       write_textures[0].dstSet);
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
            write_textures[0].dstSet;
    // Dual write-back so a live cvar flip between the two reuse gates is safe:
    // after this write BOTH schemes' state describe what was just written.
    // Always refresh edge's shader-pointer + sampler-vector snapshot (cheap).
    current_textures_vertex_shader_ = vertex_shader;
    current_written_samplers_vertex_ = current_samplers_vertex_;
    if (cvars::vulkan_texture_descriptor_reuse_edge) {
      // Edge mode didn't compute the FNV hash, so mark it invalid to force a
      // rebuild if the cvar is later flipped back to the hash gate.
      current_texture_descriptor_set_hash_valid_vertex_ = false;
    } else {
      // Hash mode: record the content hash of the set we just wrote so an
      // unchanged next draw can skip the re-write+re-bind. Valid only until an
      // invalidation (new frame / transient-pool reset / pipeline-layout
      // change).
      current_texture_descriptor_set_hash_vertex_ = texture_set_hash_vertex;
      current_texture_descriptor_set_hash_valid_vertex_ = true;
    }
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_
            ->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset,
        write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    if (cvars::vulkan_replay_stats) {
      CountTextureSetWrite(texture_set_hash_pixel);
    }
    if (texture_set_frame_reuse_frame_ == frame_current_ &&
        cvars::vulkan_texture_descriptor_set_frame_reuse &&
        !cvars::vulkan_texture_descriptor_reuse_edge) {
      texture_set_frame_reuse_.emplace(texture_set_hash_pixel,
                                       write_textures[0].dstSet);
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |=
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_
        [SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
            write_textures[0].dstSet;
    // Dual write-back (see the vertex stage above for the switch-safety
    // rationale): always refresh edge's snapshot, then keep the hash state
    // either correct-and-valid (hash mode) or marked invalid (edge mode).
    current_textures_pixel_shader_ = pixel_shader;
    current_written_samplers_pixel_ = current_samplers_pixel_;
    if (cvars::vulkan_texture_descriptor_reuse_edge) {
      current_texture_descriptor_set_hash_valid_pixel_ = false;
    } else {
      current_texture_descriptor_set_hash_pixel_ = texture_set_hash_pixel;
      current_texture_descriptor_set_hash_valid_pixel_ = true;
    }
  }
  // Write.
  if (write_descriptor_set_count) {
    for (uint32_t i = 0; i < write_descriptor_set_count; ++i) {
      if (write_descriptor_sets[i].dstSet == VK_NULL_HANDLE) {
        XELOGE(
            "UpdateBindings: write {} of {} has a null destination set (type "
            "{}, binding {}, count {}) - skipping the update",
            i, write_descriptor_set_count,
            uint32_t(write_descriptor_sets[i].descriptorType),
            write_descriptor_sets[i].dstBinding,
            write_descriptor_sets[i].descriptorCount);
        return false;
      }
    }
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count,
                               write_descriptor_sets.data(), 0, nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |=
      write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed =
      (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed &
      ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (
      xe::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        xe::tzcnt(~(descriptor_sets_remaining |
                    ((UINT32_C(1) << descriptor_set_index) - 1)));
    // When the constants set is bound as UNIFORM_BUFFER_DYNAMIC, supply one
    // dynamic offset per binding (in (set, binding) order). pDynamicOffsets
    // covers the dynamic descriptors of ALL sets in the bound range; the
    // constants set is the ONLY dynamic set in the pipeline layout
    // (shared-memory/edram is STORAGE_BUFFER, textures are SAMPLED_IMAGE/
    // SAMPLER), so any run that includes it contributes exactly these 5 offsets
    // and every other set in the run contributes zero - dynamicOffsetCount is
    // always kConstantBufferCount and the array maps 1:1 onto the constants
    // bindings (set index kDescriptorSetConstants comes before the texture sets,
    // so no earlier set in the run can be dynamic). Each offset is the aligned
    // slice offset from uniform_buffer_pool_->Request() (a multiple of
    // minUniformBufferOffsetAlignment), with slice_offset + range <= page size
    // guaranteed by the pool, so descriptor.offset(0) + dynamicOffset + range <=
    // buffer size holds.
    uint32_t dynamic_offset_count = 0;
    const uint32_t* dynamic_offsets = nullptr;
    uint32_t constants_dynamic_offsets
        [SpirvShaderTranslator::kConstantBufferCount];
    if (use_dynamic_constants_ &&
        descriptor_set_index <=
            SpirvShaderTranslator::kDescriptorSetConstants &&
        SpirvShaderTranslator::kDescriptorSetConstants <
            descriptor_set_mask_tzcnt) {
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount;
           ++i) {
        constants_dynamic_offsets[i] =
            uint32_t(current_constant_buffer_infos_[i].offset);
      }
      dynamic_offset_count = SpirvShaderTranslator::kConstantBufferCount;
      dynamic_offsets = constants_dynamic_offsets;
    }
    // TODO(Triang3l): Bind to compute for memexport emulation without vertex
    // shader memory stores.
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(),
        descriptor_set_index, descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index,
        dynamic_offset_count, dynamic_offsets);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &=
        ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

void VulkanCommandProcessor::CountTextureSetWrite(uint64_t content_hash) {
  if (replay_stats_texture_set_hashes_frame_ != frame_current_) {
    replay_stats_texture_set_hashes_.clear();
    replay_stats_texture_set_hashes_frame_ = frame_current_;
  }
  ++vk_frame_sync_stats_.texture_set_writes;
  if (!replay_stats_texture_set_hashes_.insert(content_hash).second) {
    ++vk_frame_sync_stats_.texture_set_rewrites;
  }
}

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout,
    const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it = texture_transient_descriptor_sets_free_.find(
      texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(),
        texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

#define COMMAND_PROCESSOR VulkanCommandProcessor
#include "../pm4_command_processor_implement.h"
#undef COMMAND_PROCESSOR
}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
