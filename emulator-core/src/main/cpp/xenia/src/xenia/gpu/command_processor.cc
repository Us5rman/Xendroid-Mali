/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/command_processor.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

#include "xenia/base/cvar.h"

DECLARE_int32(spin_park_mode);
DECLARE_uint32(guest_yield_sleep_us);
DECLARE_int32(apu_queue_target);

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/processor.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/packet_disassembler.h"
#include "xenia/gpu/sampler_info.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/xenos_zpd_report.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/user_module.h"

#if !defined(NDEBUG)

#define XE_ENABLE_GPU_REG_WRITE_LOGGING 1
#endif
DEFINE_bool(
    log_guest_driven_gpu_register_written_values, false,
    "Only does anything in debug builds, if set will log every write to a gpu "
    "register done by a guest. Does not log writes that are done by the CP on "
    "its own, just ones the guest makes or instructs it to make.",
    "GPU");

DEFINE_uint32(
    gpu_stall_spin_iterations, 32,
    "How many times the command processor polls the ring buffer with a cheap "
    "yield before parking on the write-pointer event when the guest has "
    "produced no commands. The spin exists to dodge wake latency when the "
    "guest is about to kick again immediately, but on ARM64 the yield is a "
    "WFE that does not actually park on a busy SoC, so every iteration burns "
    "a core at full clock. 0 parks immediately; the event is signalled by "
    "UpdateWritePointer, so no wake-up is missed either way.",
    "GPU");

DEFINE_bool(disassemble_pm4, false,
            "Only does anything in debug builds, if set will disassemble and "
            "log all PM4 packets sent to the CP.",
            "GPU");

DEFINE_bool(
    log_ringbuffer_kickoff_initiator_bts, false,
    "Only does anything in debug builds, if set will log the pseudo-stacktrace "
    "of the guest thread that wrote the new read position.",
    "GPU");

DEFINE_bool(clear_memory_page_state, false,
            "Refresh state of memory pages to enable gpu written data. "
            "Uses mostly lock-free double-buffering for minimal overhead. "
            "(Enable if rendering breaks, at a minor performance cost)",
            "GPU");
UPDATE_from_bool(clear_memory_page_state, 2026, 8, 1, 12, true);

DEFINE_bool(
    merge_tiling_bands_call_sites, true,
    "With merge_tiling_bands: tell the draws apart by the indirect buffer calls "
    "made inside the replayed scene on the way to the draw packet as well as "
    "by its address, so the objects a game draws by calling one routine each "
    "are not taken for each other - one only in a later band was skipped as "
    "drawn in an earlier band. Read per draw (debug.xendroid.band_call_sites).",
    "GPU");

DEFINE_bool(
    merge_tiling_bands_log, false,
    "With merge_tiling_bands: log every 150 band sequences the draws per "
    "sequence, the ones skipped as drawn in an earlier band, and the ones "
    "telling the draws apart by the address alone would have skipped "
    "(debug.xendroid.band_log).",
    "GPU");

DEFINE_bool(
    merge_tiling_bands, false,
    "Draw the bands of predicated tiling (the scene split into horizontal "
    "bands that fit the EDRAM, each drawn by replaying the same command "
    "buffer) as one: every draw is executed only in the first band it's "
    "predicated into, into render targets as tall as the screen, and each "
    "band is resolved from its rows - the vertex work and the fixed cost of "
    "the repeated draws are done once. Host render target path; read per "
    "frame.",
    "GPU");

DEFINE_bool(
    gpu_performance_hint, false,
    "Android: an ADPF performance hint session for the GPU command processor "
    "thread, reporting each guest frame the time the thread worked (the frame "
    "interval minus its waits for ring writes and WAIT_REG_MEM) against the "
    "average frame interval as the target, so the platform can raise the "
    "thread's clock when a frame runs late. Takes effect at startup.",
    "GPU");

DEFINE_bool(
    log_gpu_frame_time_breakdown, false,
    "Log a once-per-second breakdown of where the GPU command processor "
    "thread spends each guest frame (swap-to-swap): PM4 execution, draw "
    "processing, the swap itself, and idle stall waiting for ring writes. "
    "Diagnostic aid for frame pacing investigations.",
    "GPU");

DEFINE_string(
    occlusion_query, "fast",
    "Controls hardware occlusion query behavior for EVENT_WRITE_ZPD.\n"
    "Used for effects like lens flares, object culling, and auto-exposure.\n"
    "Titles that use QueryBatch are not currently supported and fall back to\n"
    "fake mode, regardless of this setting.\n"
    " fake: Write a fake result without asking the GPU. Safe for most games,\n"
    "       though some effects may look slightly wrong.\n"
    " fast: Ask the GPU but don't wait for the answer. Writes a cached\n"
    "       result immediately and updates it when the GPU catches up.\n"
    "       Cached results bias toward visible when guessing. (default)\n"
    " fast-alt: Variant of fast mode that keeps cached zero results for\n"
    "           unresolved reports. May improve effects relying on precise\n"
    "           visibility, but may be less stable for occlusion culling.\n"
    " strict: Ask the GPU and wait for the real result before continuing.\n"
    "         Most accurate, but may be somewhat less performant.",
    "GPU");

DEFINE_string(
    readback_resolve, "uma",
    "Controls which render-to-texture resolves are copied back into guest "
    "RAM.\n"
    " uma: No copy - the CPU reads the host-mapped shared memory directly. "
    "The default, and the only mode that works on unified-memory GPUs "
    "(Adreno). Needs host-mapped shared memory and no resolution scaling.\n"
    " fast: Copy only resolves the CPU reads back\n"
    " all: Copy every resolve\n"
    " none: Disable readback completely (improves performance).\n",
    "GPU");

DEFINE_bool(
    readback_resolve_uma_read_watch, true,
    "With readback_resolve = uma: after the first resolve into a range, copy "
    "its later resolves into guest RAM only once the CPU has read the range "
    "(the read watch of the fast mode), instead of after every resolve. The "
    "first read of a range then sees the copy of an earlier resolve. A CPU "
    "write into part of a page holding resolve output that was not copied "
    "puts older data back in the rest of the page. Need for Speed: Most "
    "Wanted resolves 122 times a frame, 77 MB its CPU never reads: at its "
    "first race's start line 18.6 -> 21.2 fps, a minute later 19.1 -> 23.2. "
    "On for every title since 2026-10-08 (others not measured). Read per "
    "resolve (debug.xendroid.uma_read_watch on Android).",
    "GPU");
UPDATE_from_bool(readback_resolve_uma_read_watch, 2026, 10, 8, 23, false);

DEFINE_int32(
    readback_resolve_uma_read_watch_min_kb, 256,
    "With readback_resolve_uma_read_watch: resolves writing less than this "
    "many KB are still copied after every resolve, read or not - few bytes, "
    "and what the game reads back may be in one without the read watch "
    "noticing (Need for Speed: Most Wanted's exposure went white from its "
    "first frames without them). Read per resolve "
    "(debug.xendroid.uma_watch_min_kb on Android).",
    "GPU");

DEFINE_bool(
    memexport_enable, false,
    "Make memory export output visible to the CPU by routing the draws that "
    "write it to a buffer aliasing guest RAM. Needed by games that read "
    "exported data on the CPU. Disabling it keeps the output in device-local "
    "memory, which is faster for the draws that consume it on the GPU. Applies "
    "at title launch. Vulkan falls back to synchronous readback from the "
    "host-mapped buffer (readback_resolve=uma) if guest RAM import is "
    "unavailable. Off by default because CPU visibility may require a GPU "
    "stall for each exporting draw.",
    "GPU");

DEFINE_bool(
    memexport_await_fences, false,
    "Wait for the GPU to finish outstanding memory export before signalling a "
    "fence the guest reads, so exported data is in guest RAM by the time the "
    "guest looks at it. Disabling it avoids the stall but games reading "
    "exported data on the CPU may see stale contents. Needs memexport_enable. "
    "Off by default here: without the host buffer the wait is a full GPU stall "
    "per fence that cannot make the output visible anyway.",
    "GPU");

DEFINE_bool(
    precise_interpolation, true,
    "Manually interpolate pixel shader inputs with barycentric coordinates to "
    "exactly match the guest and avoid hardware interpolation precision "
    "differences. Fixes noise artifacts in games like Perfect Dark and Tenchu "
    "Z "
    "that do exact equality comparisons on interpolated values. Requires "
    "fragment shader barycentric support (VK_KHR_fragment_shader_barycentric "
    "on "
    "Vulkan, SV_Barycentrics on Direct3D 12).",
    "GPU");

namespace xe {
namespace gpu {

// This should be written completely differently with support for different
// types.
void SaveGPUSetting(GPUSetting setting, uint64_t value) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      OVERRIDE_bool(clear_memory_page_state, static_cast<bool>(value));
      break;
    case GPUSetting::MemexportAwaitFences:
      OVERRIDE_bool(memexport_await_fences, static_cast<bool>(value));
      break;
  }
}

bool GetGPUSetting(GPUSetting setting) {
  switch (setting) {
    case GPUSetting::ClearMemoryPageState:
      return cvars::clear_memory_page_state;
    case GPUSetting::MemexportAwaitFences:
      return cvars::memexport_await_fences;
    default:
      return false;
  }
}

// Both mode getters are called on the GPU command processor's per-packet/
// per-draw paths; parsing the cvar string each call cost ~5% of that thread
// in profiles. Parse once, cache the enum, and have the setters (the only
// way the modes change at runtime) refresh the cache.
namespace {
std::atomic<int> readback_resolve_mode_cache{-1};
std::atomic<int> zpd_mode_cache{-1};

ReadbackResolveMode ParseReadbackResolveMode(const std::string& mode) {
  if (mode == "all") {
    return ReadbackResolveMode::kAll;
  } else if (mode == "uma") {
    return ReadbackResolveMode::kUma;
  } else if (mode == "none") {
    return ReadbackResolveMode::kDisabled;
  } else if (mode == "fast") {
    return ReadbackResolveMode::kFast;
  } else {
    // Retired values (some/full) and typos: fall back to the default rather
    // than to a mode that copies nothing on unified-memory GPUs.
    static bool logged = false;
    if (!logged) {
      logged = true;
      XELOGW("Unknown readback_resolve value '{}' - using uma", mode);
    }
    return ReadbackResolveMode::kUma;
  }
}

ReadbackResolveMode ParseReadbackResolveMode() {
  return ParseReadbackResolveMode(cvars::readback_resolve);
}

void SetReadbackResolveCvar(const std::string& mode) {
  OVERRIDE_string(readback_resolve, mode);
  readback_resolve_mode_cache.store(int(ParseReadbackResolveMode(mode)),
                                    std::memory_order_relaxed);
}

ZPDMode ParseZPDMode(const std::string& mode) {
  if (mode == "fake") {
    return ZPDMode::kFake;
  } else if (mode == "strict") {
    return ZPDMode::kStrict;
  } else if (mode == "fast") {
    return ZPDMode::kFast;
  } else if (mode == "fast-alt") {
    return ZPDMode::kFastAlt;
  } else {
    // Default to "fake" for any unrecognized value.
    return ZPDMode::kFake;
  }
}

ZPDMode ParseZPDMode() { return ParseZPDMode(cvars::occlusion_query); }
}  // namespace

ReadbackResolveMode GetReadbackResolveMode() {
  int cached = readback_resolve_mode_cache.load(std::memory_order_relaxed);
  if (XE_UNLIKELY(cached < 0)) {
    cached = int(ParseReadbackResolveMode(cvars::readback_resolve));
    readback_resolve_mode_cache.store(cached, std::memory_order_relaxed);
  }
  return ReadbackResolveMode(cached);
}

void SetReadbackResolveMode(const std::string& mode) {
  OVERRIDE_string(readback_resolve, mode);
  readback_resolve_mode_cache.store(int(ParseReadbackResolveMode(mode)),
                                    std::memory_order_relaxed);
}

ZPDMode GetZPDMode() {
  int cached = zpd_mode_cache.load(std::memory_order_relaxed);
  if (XE_UNLIKELY(cached < 0)) {
    cached = int(ParseZPDMode(cvars::occlusion_query));
    zpd_mode_cache.store(cached, std::memory_order_relaxed);
  }
  return ZPDMode(cached);
}

static void SetZPDModeCvar(const std::string& mode) {
  OVERRIDE_string(occlusion_query, mode);
  zpd_mode_cache.store(int(ParseZPDMode(mode)), std::memory_order_relaxed);
}

void InvalidateConfigModeCaches() {
  // Per-game config load (config::ReadGameConfig) writes the raw cvar strings
  // directly via IConfigVar::LoadConfigValue and does NOT go through the
  // Set*Mode setters, so the parsed-enum caches would otherwise keep the
  // values parsed from the global config and silently ignore every per-game
  // occlusion_query / readback_resolve override. Reset to -1 so the next
  // Get*Mode re-parses the now-updated cvar.
  readback_resolve_mode_cache.store(-1, std::memory_order_relaxed);
  zpd_mode_cache.store(-1, std::memory_order_relaxed);
}

using namespace xe::gpu::xenos;

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   kernel::KernelState* kernel_state)
    : reader_(nullptr, 0),
      memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      trace_writer_(graphics_system->memory()->physical_membase()),
      worker_running_(true),
      write_ptr_index_event_(xe::threading::Event::CreateAutoResetEvent(false)),
      pause_resume_event_(xe::threading::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
  assert_not_null(pause_resume_event_);
  // Parse and cache readback resolve mode once
  cached_readback_resolve_mode_ = ParseReadbackResolveMode();
  // Parse and cache ZPD mode once.
  cached_zpd_mode_ = ParseZPDMode();
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state_, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state_->GetIdleProcess()));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  // Already stopped if drained early during relaunch (stopped again at
  // teardown).
  if (!worker_thread_) {
    return;
  }

  EndTracing();

  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();

  if (wrm_writer_callback_handle_) {
    memory_->UnregisterPhysicalMemoryInvalidationCallback(
        wrm_writer_callback_handle_);
    wrm_writer_callback_handle_ = nullptr;
  }
}

void CommandProcessor::InitializeShaderStorage(
    const std::filesystem::path& cache_root, uint32_t title_id, bool blocking,
    std::function<void()> completion_callback) {
  if (completion_callback) {
    completion_callback();
  }
}

void CommandProcessor::RequestFrameTrace(
    const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming trace; cannot also trace frame.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace already pending; ignoring.");
    return;
  }
  trace_state_ = TraceState::kSingleFrame;
  trace_frame_path_ = root_path;
}

void CommandProcessor::BeginTracing(const std::filesystem::path& root_path) {
  if (trace_state_ == TraceState::kStreaming) {
    XELOGE("Streaming already active; ignoring request.");
    return;
  }
  if (trace_state_ == TraceState::kSingleFrame) {
    XELOGE("Frame trace pending; ignoring streaming request.");
    return;
  }
  // Streaming starts on the next primary buffer execute.
  trace_state_ = TraceState::kStreaming;
  trace_stream_path_ = root_path;
}

void CommandProcessor::EndTracing() {
  if (!trace_writer_.is_open()) {
    return;
  }
  assert_true(trace_state_ == TraceState::kStreaming);
  trace_state_ = TraceState::kDisabled;
  trace_writer_.Close();
}

void CommandProcessor::RestoreRegisters(uint32_t first_register,
                                        const uint32_t* register_values,
                                        uint32_t register_count,
                                        bool execute_callbacks) {
  if (first_register > RegisterFile::kRegisterCount ||
      RegisterFile::kRegisterCount - first_register < register_count) {
    XELOGW(
        "CommandProcessor::RestoreRegisters out of bounds (0x{:X} registers "
        "starting with 0x{:X}, while a total of 0x{:X} registers are stored)",
        register_count, first_register, RegisterFile::kRegisterCount);
    if (first_register > RegisterFile::kRegisterCount) {
      return;
    }
    register_count =
        std::min(uint32_t(RegisterFile::kRegisterCount) - first_register,
                 register_count);
  }
  if (execute_callbacks) {
    for (uint32_t i = 0; i < register_count; ++i) {
      WriteRegister(first_register + i, register_values[i]);
    }
  } else {
    std::memcpy(register_file_->values + first_register, register_values,
                sizeof(uint32_t) * register_count);
  }
}

void CommandProcessor::RestoreGammaRamp(
    const reg::DC_LUT_30_COLOR* new_gamma_ramp_256_entry_table,
    const reg::DC_LUT_PWL_DATA* new_gamma_ramp_pwl_rgb,
    uint32_t new_gamma_ramp_rw_component) {
  std::memcpy(gamma_ramp_256_entry_table_, new_gamma_ramp_256_entry_table,
              sizeof(reg::DC_LUT_30_COLOR) * 256);
  std::memcpy(gamma_ramp_pwl_rgb_, new_gamma_ramp_pwl_rgb,
              sizeof(reg::DC_LUT_PWL_DATA) * 3 * 128);
  gamma_ramp_rw_component_ = new_gamma_ramp_rw_component;
  OnGammaRamp256EntryTableValueWritten();
  OnGammaRampPWLValueWritten();
}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (!has_pending_fns_.load(std::memory_order_acquire) &&
      kernel::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    std::lock_guard<std::mutex> lock(pending_fns_mutex_);
    pending_fns_.push(std::move(fn));
    has_pending_fns_.store(true, std::memory_order_release);
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

void CommandProcessor::ClearReadbackBuffers() {}

void CommandProcessor::SetReadbackResolveMode(ReadbackResolveMode mode,
                                              bool persist) {
  if (cached_readback_resolve_mode_ == mode) {
    return;
  }
  // Update cached value
  cached_readback_resolve_mode_ = mode;
  // Update cvar string for UI display
  const char* mode_str = "fast";
  switch (mode) {
    case ReadbackResolveMode::kDisabled:
      mode_str = "none";
      break;
    case ReadbackResolveMode::kAll:
      mode_str = "all";
      break;
    case ReadbackResolveMode::kUma:
      mode_str = "uma";
      break;
    default:
      break;
  }
  SetReadbackResolveCvar(mode_str);

  // Save to per-game config if a title is loaded
  uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;
  if (persist && title_id != 0) {
    toml::table config_table = config::LoadGameConfig(title_id);

    if (!config_table.contains("GPU")) {
      config_table.insert("GPU", toml::table{});
    }

    auto* gpu_table = config_table["GPU"].as_table();
    if (gpu_table) {
      gpu_table->insert_or_assign("readback_resolve", mode_str);
    }

    config::SaveGameConfig(title_id, config_table);
  }
}

void CommandProcessor::SetZPDMode(ZPDMode mode) {
  if (cached_zpd_mode_ == mode) {
    return;
  }
  // Close any active query segment before the mode changes so that a
  // BeginQuery recorded under the old mode gets a matching EndQuery.
  // Without this, switching to kFake mid-frame would cause EndRenderPass
  // to skip CloseQuerySegment, leaving the query dangling.
  if (zpd_active_segment_.segment_active) {
    CloseQuerySegment();
  }
  cached_zpd_mode_ = mode;
  const char* mode_str = "fake";
  switch (mode) {
    case ZPDMode::kFast:
      mode_str = "fast";
      break;
    case ZPDMode::kFastAlt:
      mode_str = "fast-alt";
      break;
    case ZPDMode::kStrict:
      mode_str = "strict";
      break;
    default:
      break;
  }
  SetZPDModeCvar(mode_str);

  // Save to per-game config if a title is loaded.
  uint32_t title_id = kernel_state_ ? kernel_state_->title_id() : 0;
  if (title_id != 0) {
    toml::table config_table = config::LoadGameConfig(title_id);

    if (!config_table.contains("GPU")) {
      config_table.insert("GPU", toml::table{});
    }

    auto* gpu_table = config_table["GPU"].as_table();
    if (gpu_table) {
      gpu_table->insert_or_assign("occlusion_query", mode_str);
    }

    config::SaveGameConfig(title_id, config_table);
  }
}

void CommandProcessor::SetDesiredSwapPostEffect(
    SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() {
    swap_post_effect_actual_ = swap_post_effect;
  });
}

std::pair<uint32_t, uint32_t> CommandProcessor::WrmWriterWatchCallback(
    void* context_ptr, uint32_t physical_address_start, uint32_t length,
    bool exact_range) {
  // In the writing thread's fault handler, under the global critical region:
  // only record, and cheaply - this is called for every watched CPU write.
  auto& command_processor = *static_cast<CommandProcessor*>(context_ptr);
  const uint32_t address =
      command_processor.wrm_writer_address_.load(std::memory_order_acquire);
  if (address != UINT32_MAX && address >= physical_address_start &&
      address - physical_address_start < length &&
      !command_processor.wrm_writer_hit_.load(std::memory_order_relaxed)) {
    command_processor.wrm_writer_event_.host_pc =
        cpu::MMIOHandler::last_fault_host_pc();
    command_processor.wrm_writer_event_.guest_thread =
        cpu::MMIOHandler::last_fault_thread_id();
    command_processor.wrm_writer_event_.time_ns = FrameStatsNow();
    command_processor.wrm_writer_hit_.store(true, std::memory_order_release);
  }
  return std::make_pair(uint32_t(0), UINT32_MAX);
}

void CommandProcessor::WrmWriterArm(uint32_t physical_address) {
  // As TranslatePhysical reads it.
  physical_address &= 0x1FFFFFFF;
  if (!wrm_writer_callback_handle_) {
    wrm_writer_callback_handle_ =
        memory_->RegisterPhysicalMemoryInvalidationCallback(
            WrmWriterWatchCallback, this);
  }
  wrm_writer_hit_.store(false, std::memory_order_relaxed);
  wrm_writer_address_.store(physical_address, std::memory_order_release);
  memory_->EnablePhysicalMemoryAccessCallbacks(physical_address & ~0xFFFu,
                                               0x1000, true, false);
}

std::string CommandProcessor::WrmWriterTake() {
  wrm_writer_address_.store(UINT32_MAX, std::memory_order_release);
  if (!wrm_writer_hit_.load(std::memory_order_acquire)) {
    return "writer not seen";
  }
  const WrmWriterEvent event = wrm_writer_event_;
  uint32_t guest_function = 0, guest_instruction = 0;
  cpu::Processor* processor =
      kernel_state_ ? kernel_state_->processor() : nullptr;
  if (processor && processor->backend() &&
      processor->backend()->code_cache()) {
    if (cpu::GuestFunction* function =
            processor->backend()->code_cache()->LookupFunction(
                event.host_pc)) {
      guest_function = function->address();
      guest_instruction = function->MapMachineCodeToGuestAddress(
          uintptr_t(event.host_pc));
    }
  }
  return fmt::format(
      "writer thread {:08X} fn {:08X} at {:08X}, {}us before the wait saw it",
      event.guest_thread, guest_function, guest_instruction,
      (FrameStatsNow() - event.time_ns) / 1000);
}

uint64_t CommandProcessor::FrameStatsNow() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t CommandProcessor::FrameStatsBegin() {
  return cvars::log_gpu_frame_time_breakdown ? FrameStatsNow() : 0;
}

uint64_t CommandProcessor::FrameWaitBegin() {
  return (cvars::log_gpu_frame_time_breakdown || cvars::gpu_performance_hint)
             ? FrameStatsNow()
             : 0;
}

void CommandProcessor::FrameHintEndFrame() {
#if defined(__ANDROID__)
  if (!cvars::gpu_performance_hint || frame_hint_failed_) {
    return;
  }
  const uint64_t now = FrameStatsNow();
  const uint64_t wait_ns = frame_hint_wait_ns_;
  frame_hint_wait_ns_ = 0;
  if (!frame_hint_last_swap_ns_) {
    frame_hint_last_swap_ns_ = now;
    return;
  }
  const uint64_t interval_ns = now - frame_hint_last_swap_ns_;
  frame_hint_last_swap_ns_ = now;
  frame_hint_interval_sum_ns_ += interval_ns;
  ++frame_hint_interval_count_;
  if (!frame_hint_session_) {
    // Learn the frame interval first: it is the target.
    if (frame_hint_interval_count_ < 60) {
      return;
    }
    frame_hint_failed_ = true;
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
      return;
    }
    auto get_manager = reinterpret_cast<void* (*)()>(
        dlsym(lib, "APerformanceHint_getManager"));
    auto create_session =
        reinterpret_cast<void* (*)(void*, const int32_t*, size_t, int64_t)>(
            dlsym(lib, "APerformanceHint_createSession"));
    frame_hint_report_ = reinterpret_cast<int (*)(void*, int64_t)>(
        dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    frame_hint_update_target_ = reinterpret_cast<int (*)(void*, int64_t)>(
        dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    void* manager = get_manager ? get_manager() : nullptr;
    if (!manager || !create_session || !frame_hint_report_ ||
        !frame_hint_update_target_) {
      XELOGW("GpuPerformanceHint: ADPF not available");
      return;
    }
    const int32_t tid = int32_t(gettid());
    const int64_t target_ns =
        int64_t(frame_hint_interval_sum_ns_ / frame_hint_interval_count_);
    frame_hint_session_ = create_session(manager, &tid, 1, target_ns);
    if (!frame_hint_session_) {
      XELOGW("GpuPerformanceHint: createSession failed");
      return;
    }
    frame_hint_failed_ = false;
    XELOGI("GpuPerformanceHint: session for the command thread, target "
           "{:.1f} ms",
           target_ns / 1e6);
    frame_hint_interval_sum_ns_ = 0;
    frame_hint_interval_count_ = 0;
    return;
  }
  frame_hint_report_(frame_hint_session_,
                     int64_t(interval_ns > wait_ns ? interval_ns - wait_ns
                                                   : uint64_t(1)));
  // Follow the guest's frame rate.
  if (frame_hint_interval_count_ >= 120) {
    frame_hint_update_target_(
        frame_hint_session_,
        int64_t(frame_hint_interval_sum_ns_ / frame_hint_interval_count_));
    frame_hint_interval_sum_ns_ = 0;
    frame_hint_interval_count_ = 0;
  }
#endif
}

void CommandProcessor::FrameStatsEndDraw(uint64_t begin_ns) {
  if (!begin_ns) {
    return;
  }
  frame_time_stats_.draws++;
  frame_time_stats_.draw_ns += FrameStatsNow() - begin_ns;
}

void CommandProcessor::FrameStatsEndWaitRegMem(uint64_t begin_ns) {
  if (!begin_ns) {
    return;
  }
  const uint64_t wait_ns = FrameStatsNow() - begin_ns;
  frame_time_stats_.wait_reg_mem_unmet++;
  frame_time_stats_.wait_reg_mem_ns += wait_ns;
  frame_hint_wait_ns_ += wait_ns;
}

void CommandProcessor::FrameStatsEndSwap(uint64_t begin_ns) {
  if (!begin_ns) {
    return;
  }
  auto& s = frame_time_stats_;
  const uint64_t now = FrameStatsNow();
  s.swap_ns += now - begin_ns;
  s.frames++;
  if (s.last_swap_ns) {
    const uint64_t interval = now - s.last_swap_ns;
    s.interval_ns += interval;
    s.interval_max_ns = std::max(s.interval_max_ns, interval);
    s.long_intervals[0] += uint64_t(interval > 37000000);
    s.long_intervals[1] += uint64_t(interval > 50000000);
    s.long_intervals[2] += uint64_t(interval > 70000000);
  }
  s.last_swap_ns = now;
  if (!s.last_report_ns) {
    s.last_report_ns = now;
  } else if (now - s.last_report_ns >= 1000000000ull && s.frames) {
    const double f = static_cast<double>(s.frames);
    XELOGI(
        "GpuFrame: {} frames, interval avg={:.1f}ms max={:.1f}ms | per frame: "
        "exec={:.1f}ms draws={:.0f} draw={:.1f}ms swap={:.1f}ms "
        "stall={:.1f}ms | wait_reg_mem unmet={:.1f} waited={:.1f}ms | "
        "intervals >37ms={} >50ms={} >70ms={}",
        s.frames, s.interval_ns / f / 1e6, s.interval_max_ns / 1e6,
        s.exec_ns / f / 1e6, s.draws / f, s.draw_ns / f / 1e6,
        s.swap_ns / f / 1e6, s.stall_ns / f / 1e6, s.wait_reg_mem_unmet / f,
        s.wait_reg_mem_ns / f / 1e6, s.long_intervals[0], s.long_intervals[1],
        s.long_intervals[2]);
    const uint64_t keep_swap = s.last_swap_ns;
    s = FrameTimeStats();
    s.last_swap_ns = keep_swap;
    s.last_report_ns = now;
  }
}

namespace {
void BinTraceCount(std::vector<std::pair<uint64_t, uint32_t>>& counts,
                   uint64_t key) {
  for (auto& entry : counts) {
    if (entry.first == key) {
      ++entry.second;
      return;
    }
  }
  if (counts.size() < 16) {
    counts.emplace_back(key, 1);
  }
}
}  // namespace

void CommandProcessor::BinTracePoll() {
#if defined(__ANDROID__)
  auto& t = bin_trace_;
  if (++t.frames_since_poll < 30) {
    return;
  }
  t.frames_since_poll = 0;
  char value[PROP_VALUE_MAX] = {};
  // Read by the spin_park_guest_functions wait helper at run time.
  if (__system_property_get("debug.xendroid.spin_park", value) > 0 &&
      value[0] >= '0' && value[0] <= '2') {
    const int32_t mode = value[0] - '0';
    if (cvars::spin_park_mode != mode) {
      cvars::spin_park_mode = mode;
      XELOGI("debug.xendroid.spin_park: spin_park_mode = {}", mode);
    }
  }
  // Read by the Audio Worker per pump.
  if (__system_property_get("debug.xendroid.apu_queue_target", value) > 0 &&
      value[0] >= '0' && value[0] <= '9') {
    const int32_t blocks = std::min(std::atoi(value), 64);
    if (cvars::apu_queue_target != blocks) {
      cvars::apu_queue_target = blocks;
      XELOGI("debug.xendroid.apu_queue_target: apu_queue_target = {}", blocks);
    }
  }
  // Read by the guest scheduler's NtYieldExecution at run time.
  if (__system_property_get("debug.xendroid.yield_sleep_us", value) > 0 &&
      value[0] >= '0' && value[0] <= '9') {
    const uint32_t sleep_us = uint32_t(std::min(std::atoi(value), 1000));
    if (cvars::guest_yield_sleep_us != sleep_us) {
      cvars::guest_yield_sleep_us = sleep_us;
      XELOGI("debug.xendroid.yield_sleep_us: guest_yield_sleep_us = {}",
             sleep_us);
    }
  }
  // debug.xendroid.wrm_log N: log the next N PM4_WAIT_REG_MEM waits that were
  // not met on the first check (a new value re-arms it).
  if (__system_property_get("debug.xendroid.wrm_log", value) > 0) {
    const int32_t count = std::atoi(value);
    if (count != wrm_log_last_property_) {
      wrm_log_last_property_ = count;
      wrm_log_left_ = uint32_t(std::min(std::max(count, 0), 256));
      if (wrm_log_left_) {
        XELOGI("WaitRegMem: logging the next {} unmet waits", wrm_log_left_);
      }
    }
  }
  if (__system_property_get("debug.xendroid.wrm_max_us", value) > 0 &&
      value[0] >= '0' && value[0] <= '9') {
    const int32_t max_us = std::atoi(value);
    if (cvars::wait_reg_mem_backoff_max_us != max_us) {
      cvars::wait_reg_mem_backoff_max_us = max_us;
      XELOGI("debug.xendroid.wrm_max_us: wait_reg_mem_backoff_max_us = {}",
             max_us);
    }
  }
  if (__system_property_get("debug.xendroid.wait_reg_mem_backoff", value) > 0 &&
      (value[0] == '0' || value[0] == '1')) {
    const bool enabled = value[0] == '1';
    if (cvars::wait_reg_mem_backoff != enabled) {
      cvars::wait_reg_mem_backoff = enabled;
      XELOGI("debug.xendroid.wait_reg_mem_backoff: wait_reg_mem_backoff = {}",
             enabled);
    }
  }
  if (__system_property_get("debug.xendroid.fake_extents", value) > 0) {
    const uint32_t mode = uint32_t(std::min(std::max(std::atoi(value), 0), 4));
    if (mode != t.fake_extents_mode) {
      t.fake_extents_mode = mode;
      XELOGI("BinTrace: fake screen extents mode {}", mode);
    }
  }
  if (__system_property_get("debug.xendroid.pm4_bin_trace", value) <= 0) {
    return;
  }
  const int32_t frames = std::atoi(value);
  if (frames == t.last_property_value) {
    return;
  }
  t.last_property_value = frames;
  if (frames > 0) {
    t.frames_left = uint32_t(std::min(frames, 16));
    t.frame_number = 0;
    XELOGI("BinTrace: tracing the next {} frames", t.frames_left);
  }
#endif
}

void CommandProcessor::BinTraceOpcode(uint32_t opcode) {
  auto& t = bin_trace_;
  const char* note = nullptr;
  switch (opcode) {
    case PM4_INTERRUPT:
      ++t.interrupts;
      note = "I";
      break;
    case PM4_WAIT_REG_MEM:
      ++t.wait_reg_mems;
      note = "W";
      break;
    case PM4_EVENT_WRITE_EXT:
      // Run-length: only the first of a run of screen extent reports.
      if (t.frame_number == 0 && !t.sequence.empty() &&
          t.sequence.back() == 'E') {
        return;
      }
      note = "E";
      break;
    default:
      return;
  }
  if (t.frame_number == 0 && t.sequence.size() < 6000) {
    t.sequence += fmt::format(" [{}d] {}", t.draws_since_select, note);
  }
}

void CommandProcessor::OnBinSelectWritten() {
  if (!cvars::merge_tiling_bands) {
    tiling_band_ = -1;
    return;
  }
  const uint32_t select = uint32_t(bin_select_);
  if (select == UINT32_MAX) {
    tiling_band_ = -1;
    return;
  }
  if (tiling_band_ < 0) {
    if (cvars::merge_tiling_bands_log && tiling_band_stat_draws_) {
      tiling_band_stat_sum_draws_ += tiling_band_stat_draws_;
      tiling_band_stat_sum_skipped_ += tiling_band_stat_skipped_;
      tiling_band_stat_sum_rescued_ += tiling_band_stat_rescued_;
      if (++tiling_band_stat_sequences_ >= 150) {
        const double n = double(tiling_band_stat_sequences_);
        XELOGI(
            "TilingBands: {} sequences | per sequence draws={:.1f} "
            "skipped={:.1f} rescued_by_call_sites={:.1f} (call sites {})",
            tiling_band_stat_sequences_, tiling_band_stat_sum_draws_ / n,
            tiling_band_stat_sum_skipped_ / n,
            tiling_band_stat_sum_rescued_ / n,
            cvars::merge_tiling_bands_call_sites ? "on" : "off");
        tiling_band_stat_sum_draws_ = 0;
        tiling_band_stat_sum_skipped_ = 0;
        tiling_band_stat_sum_rescued_ = 0;
        tiling_band_stat_sequences_ = 0;
      }
    }
    tiling_band_stat_draws_ = 0;
    tiling_band_stat_skipped_ = 0;
    tiling_band_stat_rescued_ = 0;
    tiling_band_ = 0;
    ++tiling_band_sequence_;
    tiling_band_merge_ok_ = true;
    tiling_band_call_depth_ = ib_call_depth_;
    ClearTilingBandDraws();
  } else if (select != tiling_band_select_) {
    ++tiling_band_;
  }
  tiling_band_select_ = select;
}

void CommandProcessor::ClearTilingBandDraws() {
  tiling_band_draws_.Clear();
  tiling_band_draw_other_keys_.Clear();
}

void CommandProcessor::TilingBandDrawSet::Clear() {
  count = 0;
  // Keys of older epochs read as empty; epoch 0 is the zero fill.
  if (++epoch == 0) {
    std::fill(epochs.begin(), epochs.end(), uint32_t(0));
    epoch = 1;
  }
}

bool CommandProcessor::TilingBandDrawSet::Insert(uint64_t key) {
  if (!epoch) {
    epoch = 1;
  }
  if ((size_t(count) + 1) * 2 > keys.size()) {
    // Grow at half load, keeping the current epoch's keys.
    std::vector<uint64_t> old_keys;
    std::vector<uint32_t> old_epochs;
    old_keys.swap(keys);
    old_epochs.swap(epochs);
    const size_t size = std::max(size_t(8192), old_keys.size() * 2);
    keys.assign(size, uint64_t(0));
    epochs.assign(size, uint32_t(0));
    count = 0;
    for (size_t i = 0; i < old_keys.size(); ++i) {
      if (old_epochs[i] == epoch) {
        Insert(old_keys[i]);
      }
    }
  }
  const size_t mask = keys.size() - 1;
  // The top bits of a multiplicative hash (the address keys are dword
  // addresses, so the low bits of the product carry little).
  size_t index = size_t((key * UINT64_C(0x9E3779B97F4A7C15)) >> 40) & mask;
  for (;;) {
    if (epochs[index] != epoch) {
      keys[index] = key;
      epochs[index] = epoch;
      ++count;
      return true;
    }
    if (keys[index] == key) {
      return false;
    }
    index = (index + 1) & mask;
  }
}

bool CommandProcessor::PrepareTilingBandDraw() {
  tiling_band_draw_from_later_band_ = false;
  if (tiling_band_ < 0 || !tiling_band_merge_ok_ ||
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode ==
          xenos::EdramMode::kCopy) {
    return true;
  }
  // The address right past the packet tells the draw apart (the bands replay
  // the same command buffers), with the calls made inside the scene on the
  // way to it (beyond 32 bits, so never equal to an address alone).
  const uint32_t address = GuestReadPtrOffset();
  const bool call_sites = cvars::merge_tiling_bands_call_sites;
  const bool log = cvars::merge_tiling_bands_log;
  uint64_t call_key = address;
  if (call_sites || log) {
    const uint32_t depth = std::min(ib_call_depth_, kTilingBandMaxCallDepth);
    for (uint32_t i = tiling_band_call_depth_ + 1; i < depth; ++i) {
      call_key =
          ((call_key ^ ib_call_sites_[i]) * UINT64_C(0x9E3779B97F4A7C15)) |
          (UINT64_C(1) << 63);
    }
  }
  const bool new_draw =
      tiling_band_draws_.Insert(call_sites ? call_key : uint64_t(address));
  if (log) {
    ++tiling_band_stat_draws_;
    const bool new_other = tiling_band_draw_other_keys_.Insert(
        call_sites ? uint64_t(address) : call_key);
    const bool new_by_calls = call_sites ? new_draw : new_other;
    const bool new_by_address = call_sites ? new_other : new_draw;
    if (new_by_calls && !new_by_address && tiling_band_ > 0) {
      ++tiling_band_stat_rescued_;
    }
  }
  if (!new_draw) {
    // Executed again within the first band (a buffer it replays itself) -
    // drawn, otherwise drawn in an earlier band.
    if (tiling_band_ != 0 && log) {
      ++tiling_band_stat_skipped_;
    }
    return tiling_band_ == 0;
  }
  tiling_band_draw_from_later_band_ = tiling_band_ > 0;
  return true;
}

void CommandProcessor::BinTraceSetBin(bool is_select,
                                      uint32_t packet_guest_address) {
  auto& t = bin_trace_;
  if (is_select) {
    ++t.bin_select_writes;
    if (t.sequence.size() < 6000) {
      t.sequence += fmt::format(" [{}d] sel={:X}", t.draws_since_select,
                                bin_select_);
    }
    t.draws_since_select = 0;
  } else {
    ++t.bin_mask_writes;
    if (t.mask_writes.size() < 65536) {
      t.mask_writes.emplace_back(packet_guest_address, bin_select_, bin_mask_);
    }
  }
}

void CommandProcessor::BinTracePacket(uint32_t opcode, bool predicated,
                                      bool executed) {
  auto& t = bin_trace_;
  if (predicated) {
    ++t.packets_predicated;
    if (!executed) {
      ++t.packets_predicated_skipped;
    }
  }
}

void CommandProcessor::BinTraceDraw(bool predicated, bool executed) {
  auto& t = bin_trace_;
  if (predicated) {
    ++t.draws_predicated;
    if (!executed) {
      ++t.draws_predicated_skipped;
      return;
    }
    BinTraceCount(t.draws_by_mask, bin_mask_);
  }
  // The state of the first draw under each bin select of the first frame:
  // where the tiling bands are placed (window offset, scissors, surfaces).
  if (t.frame_number == 0 && !t.draws_since_select) {
    const RegisterFile& regs = *register_file_;
    XELOGI(
        "BinTrace band state: sel={:X} mask={:X} window_offset={:08X} "
        "window_scissor={:08X}-{:08X} screen_scissor={:08X}-{:08X} "
        "surface_info={:08X} color_info={:08X} depth_info={:08X} "
        "modecontrol={:08X} vport_y={}*y+{}",
        bin_select_, bin_mask_, regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
        regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_MODECONTROL],
        regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
        regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET));
  }
  ++t.draws;
  ++t.draws_since_select;
  BinTraceCount(t.draws_by_select, bin_select_);
}

void CommandProcessor::BinTraceIndirectBuffer(uint32_t address,
                                              uint32_t dwords, bool predicated,
                                              bool executed) {
  auto& t = bin_trace_;
  if (predicated) {
    ++t.indirect_buffers_predicated;
    if (!executed) {
      ++t.indirect_buffers_predicated_skipped;
      return;
    }
  }
  ++t.indirect_buffers;
  t.indirect_buffer_dwords += dwords;
  if (t.indirect_buffer_ranges.size() < 4096) {
    t.indirect_buffer_ranges.emplace_back(address, address + dwords * 4);
  }
}

void CommandProcessor::BinTraceExtents(uint32_t address) {
  auto& t = bin_trace_;
  if (t.extent_addresses.size() < 16384) {
    t.extent_addresses.push_back(address);
  }
}

void CommandProcessor::BinTraceNote(const char* tag, uint32_t a, uint32_t b) {
  auto& t = bin_trace_;
  if (t.frame_number == 0 && t.sequence.size() < 6000) {
    t.sequence += fmt::format(" [{}d] {}{}x{}", t.draws_since_select, tag, a, b);
  }
}

void CommandProcessor::BinTraceEndFrame() {
  auto& t = bin_trace_;
  if (!t.frames_left) {
    BinTracePoll();
    return;
  }
  // Screen extent destinations inside command buffers executed this frame
  // would mean the GPU patches its own command stream.
  uint32_t extents_in_ibs = 0;
  uint32_t extent_min = UINT32_MAX, extent_max = 0;
  std::set<uint32_t> extent_unique;
  for (uint32_t address : t.extent_addresses) {
    const uint32_t physical = address & 0x1FFFFFFF;
    extent_unique.insert(physical);
    extent_min = std::min(extent_min, physical);
    extent_max = std::max(extent_max, physical);
    for (const auto& range : t.indirect_buffer_ranges) {
      if (physical >= (range.first & 0x1FFFFFFF) &&
          physical < (range.second & 0x1FFFFFFF)) {
        ++extents_in_ibs;
        break;
      }
    }
  }
  std::string by_select, by_mask;
  for (const auto& entry : t.draws_by_select) {
    by_select += fmt::format(" {:X}:{}", entry.first, entry.second);
  }
  for (const auto& entry : t.draws_by_mask) {
    by_mask += fmt::format(" {:X}:{}", entry.first, entry.second);
  }
  XELOGI(
      "BinTrace frame {}: draws={} predicated={} (skipped {}) | packets "
      "predicated={} skipped={} | IBs={} ({} dwords) predicated={} skipped={} "
      "| bin writes: select={} mask={} | extents={} unique={} in_IBs={} "
      "range={:08X}-{:08X}",
      t.frame_number, t.draws, t.draws_predicated, t.draws_predicated_skipped,
      t.packets_predicated, t.packets_predicated_skipped, t.indirect_buffers,
      t.indirect_buffer_dwords, t.indirect_buffers_predicated,
      t.indirect_buffers_predicated_skipped, t.bin_select_writes,
      t.bin_mask_writes, t.extent_addresses.size(), extent_unique.size(),
      extents_in_ibs, extent_unique.empty() ? 0 : extent_min, extent_max);
  XELOGI("BinTrace frame {}: executed draws by bin select:{}", t.frame_number,
         by_select);
  XELOGI("BinTrace frame {}: executed predicated draws by bin mask:{}",
         t.frame_number, by_mask);
  // Same SET_BIN_MASK packet executed under several bin selects means the
  // command list is replayed per tile; a different value per tile means the
  // masks get rewritten between tiles (from the screen extents).
  {
    std::unordered_map<uint32_t, std::vector<std::pair<uint64_t, uint64_t>>>
        masks_by_address;
    for (const auto& write : t.mask_writes) {
      masks_by_address[std::get<0>(write)].emplace_back(std::get<1>(write),
                                                        std::get<2>(write));
    }
    uint32_t multi_select = 0, tile_dependent = 0;
    std::string examples;
    for (const auto& entry : masks_by_address) {
      bool several_selects = false, different = false;
      for (const auto& value : entry.second) {
        if (value.first != entry.second.front().first) {
          several_selects = true;
        }
        if (value.second != entry.second.front().second) {
          different = true;
        }
      }
      multi_select += uint32_t(several_selects);
      if (several_selects && different) {
        ++tile_dependent;
        if (tile_dependent <= 6) {
          examples += fmt::format(" {:08X}:", entry.first);
          for (const auto& value : entry.second) {
            examples += fmt::format(" {:X}->{:X}", value.first, value.second);
          }
        }
      }
    }
    XELOGI(
        "BinTrace frame {}: mask packets={} addresses={} replayed under "
        "several selects={} with per-select values={} | interrupts={} "
        "wait_reg_mem={} | fake extents mode {} | examples:{}",
        t.frame_number, t.mask_writes.size(), masks_by_address.size(),
        multi_select, tile_dependent, t.interrupts, t.wait_reg_mems,
        t.fake_extents_mode, examples);
  }
  XELOGI("BinTrace frame {}: sequence:{} [{}d]", t.frame_number, t.sequence,
         t.draws_since_select);
  const uint32_t frames_left = t.frames_left - 1;
  const uint32_t frame_number = t.frame_number + 1;
  const int32_t last_property_value = t.last_property_value;
  const uint32_t fake_extents_mode = t.fake_extents_mode;
  const uint32_t frames_since_poll = t.frames_since_poll;
  t = BinTrace();
  t.frames_left = frames_left;
  t.frame_number = frame_number;
  t.last_property_value = last_property_value;
  t.fake_extents_mode = fake_extents_mode;
  t.frames_since_poll = frames_since_poll;
}

void CommandProcessor::ThrottlePresentation() {
  // Host frame rate limiting based on framerate_limit cvar.
  const uint32_t framerate_limit = cvars::framerate_limit;

  if (framerate_limit == 0) {
    // No host frame limiting
    return;
  }

  const double target_duration_ms =
      1000.0 / static_cast<double>(framerate_limit);
  const uint64_t tick_freq = Clock::guest_tick_frequency();

  const uint64_t target_duration_ticks = static_cast<uint64_t>(
      target_duration_ms * static_cast<double>(tick_freq) / 1000.0);

  // Spin until target duration has elapsed
  while (true) {
    const uint64_t current_time = Clock::QueryGuestTickCount();
    const uint64_t time_delta = current_time - last_swap_time_;

    if (time_delta >= target_duration_ticks) {
      // If we've fallen behind by more than 2 frames, reset to catch up
      if (time_delta > target_duration_ticks * 2) {
        last_swap_time_ = current_time;
      } else {
        last_swap_time_ += target_duration_ticks;
      }
      return;
    }

    const double elapsed_ms = static_cast<double>(time_delta) /
                              (static_cast<double>(tick_freq) / 1000.0);

    const double remaining_ms = target_duration_ms - elapsed_ms;
#if XE_PLATFORM_WIN32
    // Sleep 90% of remaining, spin the rest for accuracy
    const uint64_t sleep_ns =
        static_cast<uint64_t>(remaining_ms * 1000000.0 * 0.90);
    if (sleep_ns > 0) {
      xe::threading::NanoSleep(sleep_ns);
    }
#elif XE_PLATFORM_MAC
    // Darwin's nanosleep oversleeps by 100-500us; NanoSleepPrecise spins the
    // tail so 60Hz targets don't miss their frame budget.
    const uint64_t sleep_ns = static_cast<uint64_t>(remaining_ms * 1000000.0);
    if (sleep_ns > 0) {
      xe::threading::NanoSleepPrecise(sleep_ns);
    }
#else
    const uint64_t sleep_ns = static_cast<uint64_t>(remaining_ms * 1000000.0);
    if (sleep_ns > 0) {
      xe::threading::NanoSleep(sleep_ns);
    }
#endif
  }
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    xe::FatalError("Unable to setup command processor internal state");
    return;
  }

  while (worker_running_) {
    while (has_pending_fns_.load(std::memory_order_acquire)) {
      std::function<void()> fn;
      {
        std::lock_guard<std::mutex> lock(pending_fns_mutex_);
        if (pending_fns_.empty()) {
          has_pending_fns_.store(false, std::memory_order_release);
          break;
        }
        fn = std::move(pending_fns_.front());
        pending_fns_.pop();
        if (pending_fns_.empty()) {
          has_pending_fns_.store(false, std::memory_order_release);
        }
      }
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "xe::gpu::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      const uint64_t fs_stall_begin = FrameWaitBegin();
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > cvars::gpu_stall_spin_iterations) {
          constexpr int wait_time_ms = 2;
          xe::threading::Wait(write_ptr_index_event_.get(), true,
                              std::chrono::milliseconds(wait_time_ms));
        } else {
          xe::threading::MaybeYield();
        }
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ &&
               !has_pending_fns_.load(std::memory_order_acquire) &&
               (write_ptr_index == 0xBAADF00D ||
                read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (fs_stall_begin) {
        const uint64_t stall_ns = FrameStatsNow() - fs_stall_begin;
        frame_time_stats_.stall_ns += stall_ns;
        frame_hint_wait_ns_ += stall_ns;
      }
      if (!worker_running_ ||
          has_pending_fns_.load(std::memory_order_acquire)) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // Execute. Note that we handle wraparound transparently.
    const uint64_t fs_exec_begin = FrameStatsBegin();
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);
    if (fs_exec_begin) {
      frame_time_stats_.exec_ns += FrameStatsNow() - fs_exec_begin;
    }

    // ExecutePrimaryBuffer republishes this every read_ptr_update_freq_ dwords
    // as it drains, this is the final position for the burst.
    // Keep in mind that the gpu also updates the cpu-side copy if the write
    // pointer and read pointer would be equal
    if (read_ptr_writeback_ptr_) {
      xe::store_and_swap<uint32_t>(
          memory_->TranslatePhysical(read_ptr_writeback_ptr_), read_ptr_index_);
      NotifyGuestGpuProgress();
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_.exchange(true)) {
    return;
  }

  threading::Fence fence;
  CallInThread([this, &fence]() {
    fence.Signal();
    // Event, not thread Suspend(): a Resume() landing before the worker
    // suspends would be lost forever; an early event Set() is remembered.
    threading::Wait(pause_resume_event_.get(), false);
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_.exchange(false)) {
    return;
  }

  resume_count_.fetch_add(1, std::memory_order_relaxed);
  pause_resume_event_->Set();
}

bool CommandProcessor::Save(ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  ResetZPDState();
  return true;
}

void CommandProcessor::ShutdownContext() { ResetZPDState(); }

uint32_t CommandProcessor::GuestReadPtrOffset(int32_t offset) const {
  return uint32_t(reader_.read_ptr() -
                  reinterpret_cast<uintptr_t>(memory_->physical_membase()) +
                  offset);
}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);

  std::memset(kernel_state_->memory()->TranslatePhysical(primary_buffer_ptr_),
              0, primary_buffer_size_);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr,
                                                  uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of the number of quadwords read between
  // updates of the read pointer. Kept in dwords, the unit read_ptr_index_ and
  // the write-back use. Usually 6, so 128 dwords.
  read_ptr_update_freq_ = (uint32_t(1) << std::min(block_size_log2, 19u)) * 2;
}

XE_NOINLINE XE_COLD void CommandProcessor::LogKickoffInitator(uint32_t value) {
  cpu::backend::GuestPseudoStackTrace st;

  if (logging::ShouldLog(LogLevel::Debug) &&
      kernel_state_->processor()->backend()->PopulatePseudoStacktrace(&st)) {
    logging::LoggerBatch<LogLevel::Debug> log_initiator{};

    log_initiator("Updating read ptr to {}, initiator stacktrace below\n",
                  value);

    for (uint32_t i = 0; i < st.count; ++i) {
      log_initiator("\t{:08X}\n", st.return_addrs[i]);
    }

    if (st.truncated_flag) {
      log_initiator("\t(Truncated stacktrace to {} entries)\n",
                    cpu::backend::MAX_GUEST_PSEUDO_STACKTRACE_ENTRIES);
    }
    log_initiator.submit('d');
  }
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  XE_UNLIKELY_IF(cvars::log_ringbuffer_kickoff_initiator_bts) {
    LogKickoffInitator(value);
  }
  write_ptr_index_ = value;
  write_ptr_index_event_->SetBoostPriority();
}

void CommandProcessor::LogRegisterSet(uint32_t register_index, uint32_t value) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    const RegisterInfo* reginfo = RegisterFile::GetRegisterInfo(register_index);

    if (!reginfo) {
      XELOGD("Unknown_Reg{:04X} <- {:08X}\n", register_index, value);
    } else {
      XELOGD("{} <- {:08X}\n", reginfo->name, value);
    }
  }
#endif
}

void CommandProcessor::LogRegisterSets(uint32_t base_register_index,
                                       const uint32_t* values,
                                       uint32_t n_values) {
#if XE_ENABLE_GPU_REG_WRITE_LOGGING == 1
  if (cvars::log_guest_driven_gpu_register_written_values &&
      logging::ShouldLog(LogLevel::Debug)) {
    auto target = logging::internal::GetThreadBuffer();

    auto target_ptr = target.first;

    size_t total_size = 0;

    size_t rem_size = target.second;

    for (uint32_t i = 0; i < n_values; ++i) {
      uint32_t register_index = base_register_index + i;

      uint32_t value = xe::load_and_swap<uint32_t>(&values[i]);

      const RegisterInfo* reginfo =
          RegisterFile::GetRegisterInfo(register_index);

      if (!reginfo) {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size,
                                       "Unknown_Reg{:04X} <- {:08X}\n",
                                       register_index, value);
        target_ptr = tmpres.out;
        rem_size -= tmpres.size;
        total_size += tmpres.size;

      } else {
        auto tmpres = fmt::format_to_n(target_ptr, rem_size, "{} <- {:08X}\n",
                                       reginfo->name, value);
        rem_size -= tmpres.size;
        target_ptr = tmpres.out;
        total_size += tmpres.size;
      }
    }
    logging::internal::AppendLogLine(LogLevel::Debug, 'd', total_size);
  }
#endif
}

void CommandProcessor::HandleSpecialRegisterWrite(uint32_t index,
                                                  uint32_t value) {
  RegisterFile& regs = *register_file_;
  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      // A REG4 value that is not one of the known per-frame callbacks is
      // interesting once, but "known" is a hardcoded list from one title:
      // any other game's callback looks rare on every single write. Log the
      // first sighting of each distinct value, so an install is still
      // reported without spending a formatted log line per frame forever.
      static std::atomic<uint32_t> scratch_log_budget{64};
      bool rare_install =
          scratch_reg == 4 && value != 0x0BADF00D && value != 0x83744BF8 &&
          value != 0x837C59C8;
      if (rare_install) {
        static std::mutex seen_mutex;
        static std::set<uint32_t> seen_installs;
        std::lock_guard<std::mutex> lock(seen_mutex);
        rare_install = seen_installs.insert(value).second;
      }
      if (rare_install ||
          (scratch_log_budget.load() > 0 && scratch_log_budget-- > 0)) {
        XELOGI("CP: scratch writeback REG{} value={:08X} -> phys {:08X}{}",
               scratch_reg, value, mem_addr,
               rare_install ? "  <== RARE INSTALL" : "");
      }
      xe::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      NotifyGuestGpuProgress();
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_MEM_REGs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        regs.values[index] |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color =
              regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component =
            (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
             (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl]
                                 [gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index =
              (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
              ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(
              XE_GPU_REG_DC_LUT_RW_INDEX,
              xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask =
            regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(
            XE_GPU_REG_DC_LUT_RW_INDEX,
            xe::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}
void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  // chrispy: rearrange check order, place set after checks

  if (XE_LIKELY(index < RegisterFile::kRegisterCount)) {
    register_file_->values[index] = value;

    // quick pre-test
    // todo: figure out just how unlikely this is. if very (it ought to be,
    // theres a ton of registers other than these) make this predicate
    // branchless and mark with unlikely, then make HandleSpecialRegisterWrite
    // noinline yep, its very unlikely. these ORS here are meant to be bitwise
    // ors, so that we do not do branching evaluation of the conditions (we will
    // almost always take all of the branches)

    unsigned expr = (index - XE_GPU_REG_SCRATCH_REG0 < 8) |
                    (index == XE_GPU_REG_COHER_STATUS_HOST) |
                    ((index - XE_GPU_REG_DC_LUT_RW_INDEX) <=
                     (XE_GPU_REG_DC_LUT_30_COLOR - XE_GPU_REG_DC_LUT_RW_INDEX));
    // chrispy: reordered for msvc branch probability (assumes if is taken and
    // else is not)
    if (XE_LIKELY(expr == 0)) {
      XE_MSVC_REORDER_BARRIER();

    } else {
      HandleSpecialRegisterWrite(index, value);
    }
  } else {
    XELOGW("CommandProcessor::WriteRegister index out of bounds: {}", index);
    return;
  }
}
void CommandProcessor::WriteRegistersFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = xe::load_and_swap<uint32_t>(base + i);
    this->WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(xe::RingBuffer* ring,
                                                  uint32_t base,
                                                  uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = ring->ReadAndSwap<uint32_t>();
    WriteRegister(base + i, data);
  }
}

void CommandProcessor::WriteALURangeFromRing(xe::RingBuffer* ring,
                                             uint32_t base,
                                             uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_times);
}

void CommandProcessor::WriteFetchRangeFromRing(xe::RingBuffer* ring,
                                               uint32_t base,
                                               uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_times);
}

void CommandProcessor::WriteBoolRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_times);
}

void CommandProcessor::WriteLoopRangeFromRing(xe::RingBuffer* ring,
                                              uint32_t base,
                                              uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_times);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(xe::RingBuffer* ring,
                                                   uint32_t base,
                                                   uint32_t num_times) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_times);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index,
                                            uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index,
                                              uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index,
                                             uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index,
                                                  uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}
XE_NOINLINE
void CommandProcessor::WriteOneRegisterFromRing(uint32_t base,
                                                uint32_t num_times) {
  for (uint32_t m = 0; m < num_times; m++) {
    uint32_t reg_data = reader_.ReadAndSwap<uint32_t>();
    uint32_t target_index = base;
    WriteRegister(target_index, reg_data);
  }
}
void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = xe::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  XELOGGPU("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
           base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {
  trace_writer_.Flush();
  // Only refresh completion if there is a strict ZPD retire pending so
  // PumpPendingRetire sees the latest progress without adding extra overhead.
  if (zpd_pending_retire_handle_ != kInvalidReportHandle) {
    PollCompletedSubmission();
  }
  // Give strict ZPD a chance to retire a pending report before the guest's
  // loop polls again.
  PumpPendingRetire();
}

void CommandProcessor::ReturnFromWait() {}

void CommandProcessor::InitializeTrace() {
  // Write the initial register values, to be loaded directly into the
  // RegisterFile since all registers, including those that may have side
  // effects on setting, will be saved.
  trace_writer_.WriteRegisters(
      0, reinterpret_cast<const uint32_t*>(register_file_->values),
      RegisterFile::kRegisterCount, false);

  trace_writer_.WriteGammaRamp(gamma_ramp_256_entry_table(),
                               gamma_ramp_pwl_rgb(), gamma_ramp_rw_component_);
}

CommandProcessor::PendingZPDSlot CommandProcessor::GetPendingZPDSlot(
    uint32_t slot_base, uint32_t end_record) const {
  PendingZPDSlot pending_slot;

  for (const auto& report_pair : logical_zpd_reports_) {
    const ZPDReport& report = report_pair.second;
    if (!report.ended || report.pending_segments == 0 ||
        report.slot_base != slot_base) {
      continue;
    }

    // Wait on the oldest unresolved report for this slot first.
    if (pending_slot.report_handle == kInvalidReportHandle ||
        report_pair.first < pending_slot.report_handle) {
      pending_slot.report_handle = report_pair.first;
    }

    // Slot reuse needs to be handled carefully in fast mode. Keep the biggest
    // cached delta, not the newest one. A stale zero is a lot more dangerous
    // than a stale nonzero.
    if (report.has_cached_delta) {
      if (!pending_slot.has_cached_delta ||
          report.cached_delta > pending_slot.cached_delta) {
        pending_slot.cached_delta = report.cached_delta;
      }
      pending_slot.has_cached_delta = true;
    }

    if (report.end_record) {
      auto report_cache_it =
          fast_zpd_report_cached_values_.find(report.end_record);
      if (report_cache_it != fast_zpd_report_cached_values_.end()) {
        if (!pending_slot.has_cached_delta ||
            report_cache_it->second > pending_slot.cached_delta) {
          pending_slot.cached_delta = report_cache_it->second;
        }
        pending_slot.has_cached_delta = true;
      }
    }
  }

  auto end_record_cache_it = fast_zpd_report_cached_values_.find(end_record);
  if (end_record_cache_it != fast_zpd_report_cached_values_.end()) {
    if (!pending_slot.has_cached_delta ||
        end_record_cache_it->second > pending_slot.cached_delta) {
      pending_slot.cached_delta = end_record_cache_it->second;
    }
    pending_slot.has_cached_delta = true;
  }

  return pending_slot;
}

bool CommandProcessor::BeginZPDReport(uint32_t report_address) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  // Track any delta to carry forward if the same slot is immediately reused.
  uint32_t carried_cached_delta = 0;
  bool has_carried_cached_delta = false;
  uint32_t carried_from_slot_base = 0;

  if (zpd_active_segment_.logical_active) {
    // New BEGIN while a prior report is open. Hardware has one register for
    // the query address, so a new BEGIN implicitly ends the prior one.
    if (zpd_active_segment_.end_record) {
      EndZPDReport(zpd_active_segment_.end_record, true);
    } else {
      if (cvars::occlusion_query_log) {
        XELOGI(
            "ZPD: BeginZPDReport forcing close without end record "
            "handle={}",
            zpd_active_segment_.report_handle);
      }

      carried_from_slot_base = zpd_active_segment_.slot_base;

      auto dying_report =
          logical_zpd_reports_.find(zpd_active_segment_.report_handle);
      // Carry prior delta forward so the slot doesn't briefly look occluded.
      if (dying_report != logical_zpd_reports_.end() &&
          dying_report->second.has_cached_delta) {
        carried_cached_delta = dying_report->second.cached_delta;
        has_carried_cached_delta = true;
      }

      if (zpd_active_segment_.segment_active) {
        // Deactivate the segment before DiscardZPDQuery so that
        // EndSubmission -> CloseQuerySegment does not re-enter and
        // issue a second EndQuery on the same slot.
        zpd_active_segment_.segment_active = false;
        if (DiscardZPDQuery()) {
          zpd_stats_.segments_ended++;
        } else {
          zpd_stats_.failed++;
        }
      }
      logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
      zpd_active_segment_ = {};
    }
  }

  uint32_t slot_base = XenosZPDReport::GetSlotBase(report_address);
  uint32_t begin_record = XenosZPDReport::GetBeginRecordBase(slot_base);
  uint32_t end_record = XenosZPDReport::GetEndRecordBase(slot_base);
  if (!slot_base) {
    return false;
  }

  // Resolve same slot hazards before invalidating pending writes from the prior
  // lifetime. For finished strict queries with unpolled completion, refresh now
  // and drain, avoiding unnecessary AwaitQueryResolve blocking.
  if (GetZPDMode() == ZPDMode::kStrict) {
    PollCompletedSubmission();
  } else {
    PumpQueryResolves();
  }

  PendingZPDSlot pending_slot = GetPendingZPDSlot(slot_base, end_record);

  if (pending_slot.report_handle != kInvalidReportHandle) {
    zpd_stats_.same_slot_reuse++;
    if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
      if (pending_slot.has_cached_delta) {
        carried_cached_delta = pending_slot.cached_delta;
        has_carried_cached_delta = true;
        carried_from_slot_base = slot_base;
      }
    } else {
      while (pending_slot.report_handle != kInvalidReportHandle) {
        auto report_it = logical_zpd_reports_.find(pending_slot.report_handle);
        if (report_it == logical_zpd_reports_.end()) {
          break;
        }

        uint64_t wait_for_submission =
            report_it->second.last_segment_end_submission;

        bool wait_succeeded =
            AwaitQueryResolve(pending_slot.report_handle, wait_for_submission);

        if (!wait_succeeded) {
          if (pending_slot.cached_delta != 0) {
            carried_cached_delta = pending_slot.cached_delta;
            has_carried_cached_delta = true;
            carried_from_slot_base = slot_base;
          }
          break;
        }

        PumpQueryResolves();

        pending_slot = GetPendingZPDSlot(slot_base, end_record);
      }
    }
  }

  // Bump slot sequence — invalidates pending writes from prior lifetime.
  uint64_t slot_sequence_id = ++zpd_slot_sequences_[slot_base];

  // By default, BEGIN drops the cached value so an orphaned END doesn't replay
  // something from a prior lifetime. The alternate fast path keeps it around
  // long enough for an async zero to help the next unresolved write.
  if (GetZPDMode() != ZPDMode::kFastAlt) {
    auto cache_it = fast_zpd_report_cached_values_.find(end_record);
    if (cache_it != fast_zpd_report_cached_values_.end() &&
        cache_it->second == 0) {
      fast_zpd_report_cached_values_.erase(cache_it);
    }
  }

  ReportHandle report_handle = zpd_next_report_handle_++;
  if (report_handle == kInvalidReportHandle) {
    report_handle = zpd_next_report_handle_++;
  }

  ZPDReport& logical = logical_zpd_reports_[report_handle];
  logical.slot_base = slot_base;
  logical.slot_sequence_id = slot_sequence_id;
  logical.begin_record = begin_record;
  logical.end_record = end_record;
  logical.begin_value = zpd_slot_values_[slot_base];
  logical.accumulated_samples = 0;
  logical.first_segment_end_submission = 0;
  logical.last_segment_end_submission = 0;
  logical.pending_segments = 0;
  logical.cached_delta = 0;
  logical.has_cached_delta = false;
  logical.ended = false;

  if (slot_base == carried_from_slot_base && has_carried_cached_delta) {
    logical.cached_delta = carried_cached_delta;
    logical.has_cached_delta = true;
  }

  zpd_active_segment_.report_handle = report_handle;
  zpd_active_segment_.slot_base = slot_base;
  zpd_active_segment_.begin_record = begin_record;
  zpd_active_segment_.end_record = end_record;
  zpd_active_segment_.segment_active = false;
  // Opens lazily. OpenQuerySegment will open it at the next valid opportunity.
  zpd_active_segment_.segment_pending_begin = true;
  zpd_active_segment_.logical_active = true;

  zpd_stats_.logical_begun++;
  OpenQuerySegment(true);
  return true;
}

// Guest END closes the logical lifetime, but the final value may still depend
// on in flight query segments.
bool CommandProcessor::EndZPDReport(uint32_t report_address,
                                    bool guest_forced_end) {
  if (GetZPDMode() == ZPDMode::kFake) {
    return false;
  }

  CommandProcessor::ReportHandle report_handle =
      zpd_active_segment_.report_handle;
  uint32_t stored_end_record = zpd_active_segment_.end_record;
  uint32_t report_record_base = XenosZPDReport::GetRecordBase(report_address);
  if (!report_record_base) {
    report_record_base = stored_end_record;
  }

  if (zpd_active_segment_.segment_active) {
    CloseQuerySegment();
  }

  zpd_active_segment_.segment_pending_begin = false;

  if (!report_record_base) {
    logical_zpd_reports_.erase(report_handle);
    if (cvars::occlusion_query_log) {
      XELOGI(
          "ZPD: EndZPDReport dropping handle={} with unknown record "
          "base forced={}",
          report_handle, guest_forced_end);
    }
    zpd_active_segment_ = {};
    return false;
  }

  bool resolved_immediately = false;
  uint32_t begin_record = 0;
  uint32_t begin_value = 0;
  uint32_t final_value = 0;
  uint32_t cached_delta = 0;
  bool has_cached_delta = false;

  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    zpd_active_segment_ = {};
    return false;
  }

  ZPDReport& logical = it->second;
  logical.ended = true;
  logical.end_record = report_record_base;
  begin_record = logical.begin_record;
  begin_value = logical.begin_value;

  if (logical.pending_segments == 0) {
    resolved_immediately = true;
    // Segments were already normalized as they resolved.
    final_value = static_cast<uint32_t>(
        std::min<uint64_t>(logical.accumulated_samples, UINT32_MAX));

    cached_delta = final_value;
    has_cached_delta = true;
    logical.cached_delta = cached_delta;
    logical.has_cached_delta = true;
    if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
        !fast_zpd_report_cached_values_.count(report_record_base)) {
      fast_zpd_report_cached_values_.clear();
    }
    fast_zpd_report_cached_values_[report_record_base] = cached_delta;
    final_value = cached_delta;
  } else {
    if (logical.has_cached_delta) {
      cached_delta = logical.cached_delta;
      has_cached_delta = true;
    }
    auto cache_it = fast_zpd_report_cached_values_.find(report_record_base);
    if (cache_it != fast_zpd_report_cached_values_.end()) {
      cached_delta = cache_it->second;
      has_cached_delta = true;
    }
  }

  if (resolved_immediately) {
    CommitZPDReport(logical, final_value);
    logical_zpd_reports_.erase(it);
  }

  bool has_cross_slot_end =
      stored_end_record && stored_end_record != report_record_base;
  if (has_cross_slot_end) {
    WriteZPDReport(0, stored_end_record, 0, begin_value, false);
  }

  if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
    bool write_begin = begin_record && report_record_base &&
                       begin_record != report_record_base;
    // Unknown still means visible in fast mode. Reusing cached zeroes can help
    // flares stop shining through walls, but it also tends to break occlusion
    // culling, so only do it in the alternate fast path.
    uint32_t speculative = cached_delta;
    if (!resolved_immediately) {
      speculative = 1;
      if (has_cached_delta &&
          (cached_delta != 0 || GetZPDMode() == ZPDMode::kFastAlt)) {
        speculative = cached_delta;
      }
    }
    WriteZPDReport(begin_record, report_record_base, begin_value, speculative,
                   write_begin);
  } else if (!resolved_immediately) {
    PumpQueryResolves();

    // Recheck after the drain. The report may have resolved synchronously if
    // all segments were already complete by the time we got here.
    if (!logical_zpd_reports_.count(report_handle)) {
      // OnZPDQueryResolved already committed and erased the report; nothing
      // left to defer.
    } else if (zpd_pending_retire_handle_ != report_handle) {
      zpd_pending_retire_handle_ = report_handle;
      zpd_pending_retire_stalls_ = 0;
      zpd_pending_retire_start_ms_ = Clock::QueryHostUptimeMillis();
    }
  }

  zpd_stats_.logical_ended++;
  zpd_active_segment_ = {};
  return true;
}

void CommandProcessor::OpenQuerySegment(bool can_close_submission) {
  if (GetZPDMode() == ZPDMode::kFake || zpd_force_fake_fallback_ ||
      !zpd_active_segment_.logical_active ||
      !zpd_active_segment_.segment_pending_begin || !CanOpenZPDQuery()) {
    return;
  }

  EnsureZPDQueryResources();

  if (!IsZPDQueryPoolReady()) {
    zpd_stats_.failed++;
    zpd_force_fake_fallback_ = true;
    logical_zpd_reports_.erase(zpd_active_segment_.report_handle);
    zpd_active_segment_ = {};
    return;
  }

  // Frees any slots from completed submissions before asking for new ones.
  PumpQueryResolves();

  QueryOpenResult open_result =
      OpenZPDQuery(zpd_active_segment_.report_handle, can_close_submission);
  switch (open_result) {
    case QueryOpenResult::kOpened:
      break;
    case QueryOpenResult::kDeferred:
      return;
    case QueryOpenResult::kPoolExhausted: {
      zpd_stats_.pool_exhausted++;
      if (GetZPDMode() == ZPDMode::kFast || GetZPDMode() == ZPDMode::kFastAlt) {
        // Fast mode favors forward progress over accuracy. Keep a minimal
        // accumulated value instead of waiting for a slot to become available.
        auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
        if (it != logical_zpd_reports_.end()) {
          it->second.accumulated_samples =
              std::max<uint64_t>(it->second.accumulated_samples, uint64_t{1});
        }
        zpd_active_segment_.segment_pending_begin = false;
        return;
      }
      zpd_stats_.failed++;
      return;
    }
    case QueryOpenResult::kFailed:
    default:
      zpd_stats_.failed++;
      return;
  }

  zpd_active_segment_.segment_active = true;
  zpd_active_segment_.segment_pending_begin = false;
  zpd_stats_.segments_begun++;
}

// Closes the active host segment without ending the logical report.
// BeginQuery/EndQuery can't cross D3D12 command list or Vulkan render pass
// boundaries. The result accumulates across all pieces.
void CommandProcessor::CloseQuerySegment() {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_active_segment_.segment_active) {
    return;
  }

  uint64_t submission = 0;
  if (!CloseZPDQuery(zpd_active_segment_.report_handle, submission)) {
    zpd_active_segment_.segment_active = false;
    zpd_active_segment_.scale_area = 0;
    zpd_active_segment_.segment_pending_begin =
        zpd_active_segment_.logical_active;
    zpd_stats_.failed++;
    return;
  }

  auto it = logical_zpd_reports_.find(zpd_active_segment_.report_handle);
  if (it != logical_zpd_reports_.end()) {
    // Lets PumpPendingRetire drain early segments without blocking on the
    // final segment's submission.
    if (it->second.pending_segments == 0) {
      it->second.first_segment_end_submission = submission;
    }
    it->second.pending_segments++;
    it->second.last_segment_end_submission = submission;
  }

  zpd_active_segment_.segment_active = false;
  zpd_active_segment_.scale_area = 0;

  zpd_active_segment_.segment_pending_begin =
      zpd_active_segment_.logical_active;
  zpd_stats_.segments_ended++;
}

void CommandProcessor::UpdateZPDScale(uint32_t scale_area) {
  if (GetZPDMode() == ZPDMode::kFake || !zpd_active_segment_.logical_active) {
    return;
  }
  if (zpd_active_segment_.segment_active && zpd_active_segment_.scale_area &&
      zpd_active_segment_.scale_area != scale_area) {
    // Draw scale changed in the middle of a report, so close the segment so
    // normalization divides correctly, and start a fresh one for this draw.
    CloseQuerySegment();
    OpenQuerySegment(false);
  }
  zpd_active_segment_.scale_area = scale_area;
}

void CommandProcessor::OnZPDQueryResolved(ReportHandle report_handle,
                                          uint64_t raw_samples,
                                          uint32_t scale_area) {
  auto it = logical_zpd_reports_.find(report_handle);
  if (it == logical_zpd_reports_.end()) {
    return;
  }

  ZPDReport& logical = it->second;

  if (logical.pending_segments) {
    logical.pending_segments--;
  }

  logical.accumulated_samples += NormalizeSampleCount(raw_samples, scale_area);

  if (logical.ended && logical.pending_segments == 0) {
    uint32_t final_value = static_cast<uint32_t>(
        std::min<uint64_t>(logical.accumulated_samples, UINT32_MAX));

    logical.cached_delta = final_value;
    logical.has_cached_delta = true;
    if (logical.end_record) {
      if (fast_zpd_report_cached_values_.size() >= kFastZPDCacheMaxEntries &&
          !fast_zpd_report_cached_values_.count(logical.end_record)) {
        fast_zpd_report_cached_values_.clear();
      }
      fast_zpd_report_cached_values_[logical.end_record] = final_value;
    }
    if (IsZPDReportCurrent(logical)) {
      CommitZPDReport(logical, final_value);
    }
    logical_zpd_reports_.erase(it);
  }
}

void CommandProcessor::PumpPendingRetire() {
  ReportHandle handle_to_await = zpd_pending_retire_handle_;
  if (handle_to_await == kInvalidReportHandle) {
    return;
  }

  auto logical_report = logical_zpd_reports_.find(handle_to_await);
  if (logical_report == logical_zpd_reports_.end()) {
    // If the report is already gone it retired through another path.
    // Clear so we don't spin on a handle that no longer exists.
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  uint64_t wait_for_submission =
      logical_report->second.last_segment_end_submission;
  uint64_t first_submission =
      logical_report->second.first_segment_end_submission;

  // Early segments can be retired here and, in the best case, the report
  // fully resolves without any wait.
  if (first_submission != 0 && first_submission < wait_for_submission &&
      first_submission <= GetCompletedSubmission()) {
    PumpQueryResolves();
    logical_report = logical_zpd_reports_.find(handle_to_await);
    if (logical_report == logical_zpd_reports_.end()) {
      zpd_pending_retire_handle_ = kInvalidReportHandle;
      zpd_pending_retire_stalls_ = 0;
      return;
    }
    wait_for_submission = logical_report->second.last_segment_end_submission;
  }

  if (AwaitQueryResolve(handle_to_await, wait_for_submission)) {
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
    return;
  }

  if (wait_for_submission == 0 ||
      GetCompletedSubmission() >= wait_for_submission) {
    ++zpd_pending_retire_stalls_;
  }

  // Abandon if the deadline has elapsed or the stall limit has been reached.
  // Both are checked to account for varied guest polling behavior.
  bool deadline_exceeded =
      (Clock::QueryHostUptimeMillis() - zpd_pending_retire_start_ms_ >=
       kStrictZPDRetireDeadlineMs);
  if (deadline_exceeded ||
      zpd_pending_retire_stalls_ >= kStrictZPDRetireMaxStalls) {
    if (cvars::occlusion_query_log) {
      XELOGI("ZPD: PumpPendingRetire {} handle={}, abandoning",
             deadline_exceeded ? "deadline exceeded" : "stall limit reached",
             handle_to_await);
    }
    // Write the cached delta to guest memory to avoid a sudden occlusion flash.
    if (IsZPDReportCurrent(logical_report->second)) {
      uint32_t fallback_delta = logical_report->second.cached_delta
                                    ? logical_report->second.cached_delta
                                    : 1;
      CommitZPDReport(logical_report->second, fallback_delta);
    }
    logical_zpd_reports_.erase(logical_report);
    zpd_pending_retire_handle_ = kInvalidReportHandle;
    zpd_pending_retire_stalls_ = 0;
  }
}

void CommandProcessor::WriteZPDReport(uint32_t begin_record,
                                      uint32_t end_record, uint32_t begin_value,
                                      uint32_t delta_value,
                                      bool write_begin_record) {
  xenos::xe_gpu_depth_sample_counts* begin =
      begin_record
          ? memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
                begin_record)
          : nullptr;
  if (!end_record) {
    return;
  }
  xenos::xe_gpu_depth_sample_counts* end =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(
          end_record);

  XenosZPDReport::WriteReportDelta(begin, end, begin_value, delta_value,
                                   write_begin_record);
}

void CommandProcessor::CommitZPDReport(ZPDReport& report,
                                       uint32_t delta_value) {
  uint32_t end_record =
      report.end_record ? report.end_record
                        : XenosZPDReport::GetEndRecordBase(report.slot_base);
  WriteZPDReport(report.begin_record, end_record, report.begin_value,
                 delta_value, report.begin_record != 0);

  // Advance running total so the next BeginReport on this slot picks up
  // the correct begin_value.
  uint32_t saturated_delta = XenosZPDReport::SaturateSampleCount(delta_value);
  uint32_t end_value = report.begin_value + saturated_delta;
  if (saturated_delta > UINT32_MAX - report.begin_value) {
    zpd_stats_.counter_wraps++;
  }
  zpd_slot_values_[report.slot_base] = end_value;
}

bool CommandProcessor::IsZPDReportCurrent(const ZPDReport& report) const {
  auto seq_it = zpd_slot_sequences_.find(report.slot_base);
  uint64_t current_seq =
      seq_it != zpd_slot_sequences_.end() ? seq_it->second : 0;
  return current_seq == report.slot_sequence_id;
}

uint32_t CommandProcessor::NormalizeSampleCount(uint64_t samples,
                                                uint32_t scale_area) {
  if (samples == 0) {
    return 0;
  }

  if (scale_area & kZPDScaleHalfSamples) {
    // Each host sample stands for two of the guest's.
    samples <<= 1;
  }
  uint64_t scale = scale_area & ~kZPDScaleHalfSamples;
  // Round, don't truncate. 1 guest sample at 2x = 4 host samples, need >= 1.
  uint64_t normalized = scale <= 1 ? samples : (samples + (scale >> 1)) / scale;

  return static_cast<uint32_t>(std::min<uint64_t>(normalized, UINT32_MAX));
}

#define COMMAND_PROCESSOR CommandProcessor
#include "pm4_command_processor_implement.h"
}  // namespace gpu
}  // namespace xe
