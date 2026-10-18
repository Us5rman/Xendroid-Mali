/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"

#include <limits>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/xma_decoder.h"
#include "xenia/base/assert.h"
#include "xenia/base/byte_stream.h"
#if XE_PLATFORM_xendroid
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#endif

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/base/ring_buffer.h"
#include "xenia/base/string_buffer.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

DECLARE_bool(apu_aaudio_log_stats);

DEFINE_uint32(apu_max_queued_frames, 8,
              "Allows changing max buffered audio frames to reduce audio "
              "delay. Lowering this value might cause performance issues. "
              "Value range: [4-64]",
              "APU");
DEFINE_bool(apu_pump_topup, true,
            "Let the audio buffer refill at up to twice real time while it is "
            "running low, instead of only one frame per 5.33ms interval.",
            "APU");
DEFINE_bool(
    apu_adaptive_queue, true,
    "With apu_pump_topup and a driver that reports its queue (AAudio): after a "
    "stall that used up half of what the queue covers - a pump that late, or "
    "a guest audio callback that slow - or a queue found with at most one "
    "block left, in a pump that brought a block, refill that client's queue "
    "2 blocks deeper (up to apu_max_queued_frames - 1, once per 500 ms), and "
    "come back one block after 20 s without that. "
    "Under load the Audio Worker can wait 10-60 ms for a core or for the "
    "guest - Forza Horizon's while driving into things, in runs of seconds - "
    "which emptied the usual 4-5 blocks (21-27 ms) into gaps; the added "
    "latency comes only after such stalls. "
    "debug.xendroid.apu_adaptive_queue switches it at run time on Android.",
    "APU");
DEFINE_bool(
    apu_performance_hint, true,
    "Register an ADPF performance hint session over the guest dispatch and "
    "audio threads so the power HAL holds clocks for the audio deadline.",
    "APU");
DEFINE_int32(
    apu_queue_target, 0,
    "Blocks of 5.33 ms that apu_pump_topup keeps queued in a driver that "
    "reports its queue (AAudio), 4 to apu_max_queued_frames - 1 (fewer would "
    "sit where AAudio's dynamic rate bends pitch); 0 - half of "
    "apu_max_queued_frames, at least 4. Each block is latency, and covers "
    "that much of a stall of the Audio Worker or the guest's callback; with "
    "4 here, more apu_max_queued_frames only leaves apu_adaptive_queue more "
    "room. debug.xendroid.apu_queue_target switches it at run time on "
    "Android.",
    "APU");
UPDATE_from_uint32(apu_max_queued_frames, 2024, 8, 31, 20, 64);

namespace xe {
namespace apu {

AudioSystem::AudioSystem(cpu::Processor* processor)
    : memory_(processor->memory()),
      processor_(processor),
      worker_running_(false) {
  queued_frames_ = std::clamp(cvars::apu_max_queued_frames,
                              static_cast<uint32_t>(kMinimumQueuedFrames),
                              static_cast<uint32_t>(kMaximumQueuedFrames));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = xe::threading::Semaphore::Create(0, queued_frames_);
  }
  pending_work_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(pending_work_event_);

  xma_decoder_ = std::make_unique<xe::apu::XmaDecoder>(processor_);

  resume_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(kernel::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state->GetSystemProcess()));
  // As we run audio callbacks the debugger must be able to suspend us.
  worker_thread_->set_can_debugger_suspend(true);
  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();
  // Set high priority for this thread for better pacing.
  worker_thread_->SetPriority(24);
  return X_STATUS_SUCCESS;
}

#if XE_PLATFORM_xendroid
// Reporting pump duration against the block deadline is the unprivileged way
// to stop a power profile parking cores below what the mixer needs.
// dlsym'd: ADPF is API 33+, minSdk is 29.
class AudioPerformanceHint {
 public:
  void ReportPump(uint64_t actual_us) {
    if (state_ == State::kDisabled) {
      return;
    }
    if (state_ == State::kUninitialized) {
      Initialize();
      if (state_ == State::kDisabled) {
        return;
      }
    }
    report_(session_, static_cast<int64_t>(actual_us) * 1000);
  }

 private:
  enum class State { kUninitialized, kActive, kDisabled };

  void Initialize() {
    state_ = State::kDisabled;
    if (!cvars::apu_performance_hint) {
      return;
    }
    void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) {
      return;
    }
    auto get_manager = reinterpret_cast<void* (*)()>(
        dlsym(lib, "APerformanceHint_getManager"));
    auto create_session = reinterpret_cast<void* (*)(
        void*, const int32_t*, size_t, int64_t)>(
        dlsym(lib, "APerformanceHint_createSession"));
    report_ = reinterpret_cast<int (*)(void*, int64_t)>(
        dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    if (!get_manager || !create_session || !report_) {
      return;
    }
    void* manager = get_manager();
    if (!manager) {
      return;
    }
    // One thread per name, the oldest (lowest tid): a short-lived thread can
    // carry a worker's name, and a session naming a thread that has exited by
    // the time it is created fails as a whole (2 of 13 Forza launches: 10
    // threads instead of 9).
    std::map<std::string, int32_t> tid_by_name;
    if (DIR* dir = opendir("/proc/self/task")) {
      while (dirent* entry = readdir(dir)) {
        int32_t tid = atoi(entry->d_name);
        if (tid <= 0) {
          continue;
        }
        char comm_path[64];
        snprintf(comm_path, sizeof(comm_path), "/proc/self/task/%d/comm", tid);
        char comm[32] = {0};
        if (FILE* f = fopen(comm_path, "r")) {
          fgets(comm, sizeof(comm), f);
          fclose(f);
        }
        if (!strncmp(comm, "Guest CPU", 9) || !strncmp(comm, "Audio Worker", 12) ||
            !strncmp(comm, "XMA Decoder", 11)) {
          auto it = tid_by_name.emplace(comm, tid).first;
          it->second = std::min(it->second, tid);
        }
      }
      closedir(dir);
    }
    std::vector<int32_t> tids;
    for (const auto& name_tid : tid_by_name) {
      tids.push_back(name_tid.second);
    }
    if (tids.empty()) {
      return;
    }
    session_ = create_session(manager, tids.data(), tids.size(),
                              AudioSystem::kAudioPumpInterval * int64_t(1000));
    if (!session_) {
      // Once more with the threads still alive.
      std::vector<int32_t> alive;
      for (int32_t tid : tids) {
        char task_path[48];
        snprintf(task_path, sizeof(task_path), "/proc/self/task/%d", tid);
        if (access(task_path, F_OK) == 0) {
          alive.push_back(tid);
        }
      }
      if (!alive.empty() && alive.size() != tids.size()) {
        tids.swap(alive);
        session_ =
            create_session(manager, tids.data(), tids.size(),
                           AudioSystem::kAudioPumpInterval * int64_t(1000));
      }
    }
    if (!session_) {
      XELOGW("AudioPerformanceHint: createSession failed ({} threads)",
             tids.size());
      return;
    }
    XELOGI("AudioPerformanceHint: session over {} threads, target {}us",
           tids.size(), AudioSystem::kAudioPumpInterval);
    state_ = State::kActive;
  }

  State state_ = State::kUninitialized;
  void* session_ = nullptr;
  int (*report_)(void*, int64_t) = nullptr;
};

// Reports the first failure rather than dropping it: an app that is not
// allowed to renice would otherwise look like it had succeeded.
void ApplyAudioThreadPriority(const char* what) {
  static constexpr int kNice = -12;
  errno = 0;
  if (setpriority(PRIO_PROCESS, 0, kNice) != 0) {
    static std::atomic<bool> reported{false};
    bool expected = false;
    if (reported.compare_exchange_strong(expected, true)) {
      XELOGW("{}: setpriority({}) failed: errno {} ({})", what, kNice, errno,
             std::strerror(errno));
    }
    return;
  }
  const int applied = getpriority(PRIO_PROCESS, 0);
  static std::atomic<bool> logged{false};
  bool expected = false;
  if (logged.compare_exchange_strong(expected, true)) {
    XELOGI("{}: nice set to {}", what, applied);
  }
}

// The calling thread's time waiting for a core while runnable, from
// /proc/thread-self/schedstat (its second field), in microseconds; 0 if the
// kernel does not keep it. Open from the thread it measures.
class RunQueueWait {
 public:
  RunQueueWait()
      : fd_(open("/proc/thread-self/schedstat", O_RDONLY | O_CLOEXEC)) {}
  ~RunQueueWait() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  uint64_t TotalUs() const {
    if (fd_ < 0) {
      return 0;
    }
    char text[96];
    const ssize_t size = pread(fd_, text, sizeof(text) - 1, 0);
    if (size <= 0) {
      return 0;
    }
    text[size] = '\0';
    unsigned long long cpu_ns = 0, wait_ns = 0;
    if (std::sscanf(text, "%llu %llu", &cpu_ns, &wait_ns) != 2) {
      return 0;
    }
    return wait_ns / 1000;
  }

 private:
  int fd_;
};
#endif

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

#if XE_PLATFORM_xendroid
  // Set here, not at the creation site: XThread applies its own priority
  // after Create, and its kNormal maps to nice 4, below the Android default.
  // Re-applied in the loop below, because a later set_priority would undo it.
  ApplyAudioThreadPriority("Audio Worker");
  auto priority_last = std::chrono::steady_clock::now();
#endif
#if XE_PLATFORM_xendroid
  AudioPerformanceHint audio_perf_hint;
#endif

  // apu_aaudio_log_stats: what the guest made of the pumps, once a second -
  // whether it keeps real time (187.5 frames a second) and, when it does not,
  // whether its callback was slow, ran late or returned without a frame.
  struct PumpStats {
    uint32_t pumps = 0;
    uint32_t pumps_without_frame = 0;
    uint32_t topups = 0;
    uint32_t without_slot = 0;
    uint64_t callback_us_sum = 0;
    uint64_t callback_us_max = 0;
    uint64_t late_us_max = 0;
    // Android: the callback's CPU time, the slowest callback's alone, and the
    // core each pump ran on.
    uint64_t callback_cpu_us_sum = 0;
    uint64_t slowest_callback_cpu_us = 0;
    uint32_t pumps_on_cpu[8] = {};
    // Android: time runnable but waiting for a core - in the slowest
    // callback, the most in one callback - the rest of a slow callback's wall
    // time beyond its CPU time is the guest waiting.
    uint64_t slowest_callback_run_wait_us = 0;
    uint64_t callback_run_wait_us_max = 0;
  } pump_stats;
  auto pump_stats_last = std::chrono::steady_clock::now();
#if XE_PLATFORM_xendroid
  // apu_aaudio_log_stats: the worker's whole wait for a core, per second
  // (late wake-ups for the pacing included).
  const RunQueueWait run_queue_wait;
  uint64_t run_wait_us_before = run_queue_wait.TotalUs();
#endif
  auto frames_submitted_total = [this]() {
    uint64_t total = 0;
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      total += clients_[i].frames_submitted.load();
    }
    return total;
  };
  uint64_t pump_stats_frames_before = frames_submitted_total();
  // Counters that a client's restart (or a failed read) sets back count
  // nothing for that second instead of wrapping.
  auto counted_since = [](uint64_t now, uint64_t before) -> uint64_t {
    return now > before ? now - before : 0;
  };

  // apu_adaptive_queue, per client, started over at its first pump and after
  // a pause: blocks refilled beyond the usual depth, when they were last
  // added, when the last stall was, and whether the queue has reached the
  // usual depth since that start.
  struct AdaptiveQueue {
    size_t extra = 0;
    std::chrono::steady_clock::time_point grown;
    std::chrono::steady_clock::time_point since;
    bool filled = false;
  };
  AdaptiveQueue adaptive_queues[kMaximumClientCount];

  // The host mixer releases a client's semaphore on its own coarse cadence,
  // but Xenos audio subsystem operates at 5.333ms interval (see
  // xaudio2_audio_driver.cc) Interval scales inversely with guest_time_scalar.
  // We therefore pace pumps to each client's next_pump_us deadline and use
  // the semaphore only as back-pressure: a frame is submitted only if
  // a host output slot is free, otherwise it is dropped (the host queue
  // is full, so it is already well buffered).
  while (worker_running_) {
#if XE_PLATFORM_xendroid
    {
      const auto priority_now = std::chrono::steady_clock::now();
      if (priority_now - priority_last >= std::chrono::seconds(1)) {
        priority_last = priority_now;
        ApplyAudioThreadPriority("Audio Worker");
      }
    }
#endif
    if (cvars::apu_aaudio_log_stats) {
      const auto stats_now = std::chrono::steady_clock::now();
      if (stats_now - pump_stats_last >= std::chrono::seconds(1)) {
        pump_stats_last = stats_now;
        const uint64_t frames_now = frames_submitted_total();
        uint32_t frames_silent = 0;
        uint32_t silences_after_sound = 0;
        for (size_t i = 0; i < kMaximumClientCount; ++i) {
          frames_silent += clients_[i].frames_silent.exchange(0);
          silences_after_sound += clients_[i].silences_after_sound.exchange(0);
        }
        XELOGI(
            "AudioWorker: {} pumps ({} without a frame, {} top-ups, {} "
            "deadlines without a free slot), {} frames, guest callback {} us "
            "avg {} max, late {} us max; {} silent frames from the guest, {} "
            "of them after sound",
            pump_stats.pumps, pump_stats.pumps_without_frame,
            pump_stats.topups, pump_stats.without_slot,
            counted_since(frames_now, pump_stats_frames_before),
            pump_stats.pumps
                ? pump_stats.callback_us_sum / pump_stats.pumps
                : 0,
            pump_stats.callback_us_max, pump_stats.late_us_max, frames_silent,
            silences_after_sound);
#if XE_PLATFORM_xendroid
        const uint32_t* on_cpu = pump_stats.pumps_on_cpu;
        XELOGI(
            "AudioWorker: guest callback CPU time {} us avg, {} us in the "
            "slowest; pumps by core {},{},{},{},{},{},{},{}",
            pump_stats.pumps
                ? pump_stats.callback_cpu_us_sum / pump_stats.pumps
                : 0,
            pump_stats.slowest_callback_cpu_us, on_cpu[0], on_cpu[1],
            on_cpu[2], on_cpu[3], on_cpu[4], on_cpu[5], on_cpu[6], on_cpu[7]);
        const uint64_t run_wait_us_now = run_queue_wait.TotalUs();
        XELOGI(
            "AudioWorker: waiting for a core {} us in the slowest callback, "
            "{} us at most in one, {} us in the second",
            pump_stats.slowest_callback_run_wait_us,
            pump_stats.callback_run_wait_us_max,
            counted_since(run_wait_us_now, run_wait_us_before));
        run_wait_us_before = run_wait_us_now;
#endif
        pump_stats = PumpStats();
        pump_stats_frames_before = frames_now;
      }
    }

    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    size_t client_index = kMaximumClientCount;
    uint64_t earliest_pump_us = std::numeric_limits<uint64_t>::max();
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);

      for (size_t i = 0; i < kMaximumClientCount; ++i) {
        if (!clients_[i].in_use ||
            clients_[i].next_pump_us >= earliest_pump_us) {
          continue;
        }
        earliest_pump_us = clients_[i].next_pump_us;
        client_index = i;
      }

      if (client_index != kMaximumClientCount) {
        const double scalar = xe::Clock::guest_time_scalar();
        const uint64_t min_us =
            scalar > 0.0 ? static_cast<uint64_t>(kAudioPumpInterval / scalar)
                         : kAudioPumpInterval;
        clients_[client_index].next_pump_us =
            (earliest_pump_us > now ? earliest_pump_us : now) + min_us;
      }
    }

    // Test the pause flag on every iteration, not just after a wait on
    // pending_work_event_ returns. When the worker is running behind, the
    // pacing wait below is skipped entirely, so a worker that only noticed a
    // pause through that wait would never acknowledge one and Pause() would
    // block on pause_fence_ forever.
    if (paused_) {
      pause_fence_.Signal();
      xe::threading::Wait(resume_event_.get(), false);
      // The deadlines the pause skipped are not lateness: start them over, as
      // for a new client.
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        for (size_t i = 0; i < kMaximumClientCount; ++i) {
          clients_[i].next_pump_us = 0;
        }
      }
      continue;
    }

    // No clients yet: park until one registers or we're told to stop.
    if (client_index == kMaximumClientCount) {
      xe::threading::Wait(pending_work_event_.get(), true);
      continue;
    }

    // Pace to kAudioIntervalSlack ahead of the deadline.
    const uint64_t wake_target_us = earliest_pump_us > kAudioIntervalSlack
                                        ? earliest_pump_us - kAudioIntervalSlack
                                        : 0;
    if (wake_target_us > now) {
      const std::chrono::milliseconds timeout((wake_target_us - now) / 1000);
      auto result =
          xe::threading::Wait(pending_work_event_.get(), true, timeout);
      if (result == xe::threading::WaitResult::kSuccess) {
        // Woken by a pause or a shutdown; both are handled at the top of the
        // loop, which also re-tests worker_running_ first so a shutdown during
        // a pause does not park on resume_event_.
        continue;
      }

      const uint64_t now_precise = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (wake_target_us > now_precise) {
        xe::threading::NanoSleepPrecise((wake_target_us - now_precise) * 1000);
      }
    }
    // How late this pump is, for apu_adaptive_queue and the statistics. A new
    // client's first deadline is 0 - not late, just unset.
    uint64_t pump_late_us = 0;
    {
      const uint64_t pump_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count());
      if (earliest_pump_us && pump_us > earliest_pump_us) {
        pump_late_us = pump_us - earliest_pump_us;
      }
    }
    pump_stats.late_us_max = std::max(pump_stats.late_us_max, pump_late_us);

    // Submit only if the host has a free output slot; otherwise drop (the host
    // queue is full, so it is already well buffered).
    if (xe::threading::Wait(client_semaphores_[client_index].get(), false,
                            std::chrono::milliseconds(0)) !=
        xe::threading::WaitResult::kSuccess) {
      ++pump_stats.without_slot;
      continue;
    }

    // UnregisterClient waits on this after clearing in_use.
    std::lock_guard<std::mutex> cb_lk(clients_[client_index].callback_mutex);

    uint32_t client_callback = 0;
    uint32_t client_callback_arg = 0;
    {
      std::lock_guard<std::mutex> clients_lock(clients_mutex_);
      if (clients_[client_index].in_use) {
        client_callback = clients_[client_index].callback;
        client_callback_arg = clients_[client_index].wrapped_callback_arg;
      }
    }

    if (client_callback) {
      SCOPE_profile_cpu_i("apu", "xe::apu::AudioSystem->client_callback");
      uint64_t args[] = {client_callback_arg};
      uint32_t submitted_before =
          clients_[client_index].frames_submitted.load();
      // The guest callback, timed: the wall time, and on Android, for the
      // statistics only (system calls and a read of procfs), the thread's CPU
      // time - less than the wall time when the callback waits or the thread
      // is preempted -, its wait for a core and the core it ran on.
      auto run_callback = [&]() {
#if XE_PLATFORM_xendroid
        const bool timed = cvars::apu_aaudio_log_stats;
        timespec cpu_begin = {};
        int cpu = -1;
        uint64_t run_wait_begin_us = 0;
        if (timed) {
          clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_begin);
          cpu = sched_getcpu();
          run_wait_begin_us = run_queue_wait.TotalUs();
        }
#endif
        const auto begin = std::chrono::steady_clock::now();
        processor_->Execute(worker_thread_->thread_state(), client_callback,
                            args, xe::countof(args));
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - begin)
                .count());
        ++pump_stats.pumps;
        pump_stats.callback_us_sum += us;
#if XE_PLATFORM_xendroid
        if (timed) {
          timespec cpu_end;
          clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
          const uint64_t cpu_us = static_cast<uint64_t>(
              (int64_t(cpu_end.tv_sec) - int64_t(cpu_begin.tv_sec)) * 1000000 +
              (int64_t(cpu_end.tv_nsec) - int64_t(cpu_begin.tv_nsec)) / 1000);
          pump_stats.callback_cpu_us_sum += cpu_us;
          const uint64_t run_wait_us =
              counted_since(run_queue_wait.TotalUs(), run_wait_begin_us);
          pump_stats.callback_run_wait_us_max =
              std::max(pump_stats.callback_run_wait_us_max, run_wait_us);
          if (us >= pump_stats.callback_us_max) {
            pump_stats.slowest_callback_cpu_us = cpu_us;
            pump_stats.slowest_callback_run_wait_us = run_wait_us;
          }
          if (cpu >= 0 && cpu < int(xe::countof(pump_stats.pumps_on_cpu))) {
            ++pump_stats.pumps_on_cpu[cpu];
          }
        }
#endif
        pump_stats.callback_us_max = std::max(pump_stats.callback_us_max, us);
        return us;
      };
      const uint64_t exec_us = run_callback();
#if XE_PLATFORM_xendroid
      audio_perf_hint.ReportPump(exec_us);
#endif
      bool submitted =
          clients_[client_index].frames_submitted.load() != submitted_before;
      if (!submitted) {
        ++pump_stats.pumps_without_frame;
      }

      // Deadline pacing alone submits one block per interval, matching the
      // drain rate, so the sink cannot refill after an underrun. Add at most
      // one extra frame per interval while it is below half full: the guest
      // then runs at most twice real time, briefly. Credits alone are not a
      // usable signal for this - a guest that submits several frames from one
      // callback, or from another thread, breaks the one-credit-one-frame
      // assumption and the buffer gets driven far past real time, which some
      // titles answer with corrupted audio.
      AudioDriver* driver = nullptr;
      {
        std::lock_guard<std::mutex> clients_lock(clients_mutex_);
        if (clients_[client_index].in_use) {
          driver = clients_[client_index].driver;
        }
      }
      // Refill to above the driver's rate-control depth, or playback slows at
      // a depth this loop is happy to sit at.
      const size_t topup_base =
          cvars::apu_queue_target > 0
              ? std::max<size_t>(
                    std::min<size_t>(size_t(cvars::apu_queue_target),
                                     queued_frames_ - 1),
                    4)
              : std::max<size_t>(queued_frames_ / 2, 4);
      // 0 from a driver that does not report it, which keeps topping up.
      const size_t depth = driver ? driver->GetQueuedFrameCount() : 0;
      // apu_adaptive_queue: deeper after a stall - this pump this late, or
      // the guest's callback this slow - that used up half of what the queue
      // covers, or after finding it nearly dry. Stalls come in runs (a heavy
      // scene), so the next one finds the deeper queue. Only pumps that
      // brought a block count: the top-up follows those alone, and a guest
      // with nothing to submit drains the queue without any stall.
      AdaptiveQueue& adaptive = adaptive_queues[client_index];
      if (!earliest_pump_us) {
        // The client's first pump, or the first after a pause.
        adaptive = AdaptiveQueue();
      }
      if (cvars::apu_adaptive_queue && cvars::apu_pump_topup && driver &&
          driver->ReportsQueuedFrameCount()) {
        const auto adapt_now = std::chrono::steady_clock::now();
        const size_t deepest = queued_frames_ - 1;
        const size_t target = topup_base + adaptive.extra;
        const uint64_t stall_us = std::max(pump_late_us, exec_us);
        // Until the queue first reaches the usual depth it is still filling:
        // a queue that low, or a slow first callback, is the start, not a
        // stall.
        if (depth >= topup_base) {
          adaptive.filled = true;
        }
        const bool stressed =
            submitted && adaptive.filled &&
            (depth <= 1 || stall_us * 2 >= target * kAudioPumpInterval);
        if (stressed && target < deepest &&
            adapt_now - adaptive.grown >= std::chrono::milliseconds(500)) {
          adaptive.extra = std::min(adaptive.extra + 2, deepest - topup_base);
          adaptive.grown = adapt_now;
          adaptive.since = adapt_now;
          XELOGI(
              "AudioWorker: a {} us stall with {} blocks queued, refilling to "
              "{} blocks",
              stall_us, depth, topup_base + adaptive.extra);
        } else if (stressed) {
          adaptive.since = adapt_now;
        } else if (adaptive.extra &&
                   adapt_now - adaptive.since >= std::chrono::seconds(20)) {
          --adaptive.extra;
          adaptive.since = adapt_now;
        }
      } else {
        adaptive = AdaptiveQueue();
      }
      // The extra stays within the credits when the base is raised at run time.
      const size_t topup_below =
          std::max(topup_base, std::min<size_t>(topup_base + adaptive.extra,
                                                queued_frames_ - 1));
      if (submitted && cvars::apu_pump_topup && driver && !paused_ &&
          worker_running_ && depth < topup_below &&
          xe::threading::Wait(client_semaphores_[client_index].get(), false,
                              std::chrono::milliseconds(0)) ==
              xe::threading::WaitResult::kSuccess) {
        run_callback();
        ++pump_stats.topups;
      }
    }
  }
  XELOGW("AudioWorker: loop EXITED (worker_running_={})",
         worker_running_.load());
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (int i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::StopWorker() {
  worker_running_ = false;
  pending_work_event_->Set();
  // A worker parked on resume_event_ never sees worker_running_ go false.
  if (paused_) {
    Resume();
  }
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }
}

void AudioSystem::Shutdown() {
  StopWorker();

  // Unregister all active clients to shut down their audio drivers before
  // the semaphores are destroyed with this AudioSystem.
  uint32_t wrapped_callback_args[kMaximumClientCount] = {};
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      if (clients_[i].in_use) {
        DestroyDriver(clients_[i].driver);
        wrapped_callback_args[i] = clients_[i].wrapped_callback_arg;
        clients_[i].driver = nullptr;
        clients_[i].callback = 0;
        clients_[i].callback_arg = 0;
        clients_[i].wrapped_callback_arg = 0;
        clients_[i].in_use = false;
      }
    }
  }
  // Outside clients_mutex_, as the heap takes the global lock.
  for (uint32_t wrapped_callback_arg : wrapped_callback_args) {
    if (wrapped_callback_arg) {
      memory()->SystemHeapFree(wrapped_callback_arg);
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg,
                                     size_t* out_index) {
  // Outside clients_mutex_, as the heap takes the global lock.
  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  xe::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  std::unique_lock<std::mutex> clients_lock(clients_mutex_);

  auto index = FindFreeClient();
  if (index < 0) {
    // Every client slot is taken: indexing the semaphores with -1 wrote outside
    // them (the assert is compiled out of release builds).
    XELOGE("AudioSystem::RegisterClient: all {} clients in use",
           kMaximumClientCount);
    clients_lock.unlock();
    memory()->SystemHeapFree(ptr);
    return X_STATUS_UNSUCCESSFUL;
  }

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    XELOGE("AudioSystem::RegisterClient: CreateDriver failed for index={}",
           index);
    // Take the credits back, as UnregisterClient does: left in the semaphore,
    // the next client of this slot found it full and its own release failed.
    while (xe::threading::Wait(client_semaphore, false,
                               std::chrono::milliseconds(0)) ==
           xe::threading::WaitResult::kSuccess) {
    }
    clients_lock.unlock();
    memory()->SystemHeapFree(ptr);
    return result;
  }
  assert_not_null(driver);
  XELOGI(
      "AudioSystem::RegisterClient: driver created for index={}, driver={:p}",
      index, (void*)driver);

  clients_[index].driver = driver;
  clients_[index].callback = callback;
  clients_[index].callback_arg = callback_arg;
  clients_[index].wrapped_callback_arg = ptr;
  clients_[index].in_use = true;
  clients_[index].next_pump_us = 0;
  clients_[index].last_frame_silent = true;
  clients_[index].frames_submitted.store(0);
  clients_[index].frames_processed.store(0);
  clients_[index].frames_dropped.store(0);

  // Wake the worker so it re-scans and starts pacing this client immediately.
  pending_work_event_->Set();

  XELOGI("AudioSystem::RegisterClient: client {} registered successfully",
         index);

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, float* samples) {
  SCOPE_profile_cpu_f("apu");

  std::lock_guard<std::mutex> clients_lock(clients_mutex_);
  assert_true(index < kMaximumClientCount);
  if (index >= kMaximumClientCount || !clients_[index].in_use ||
      !clients_[index].driver) {
    XELOGW(
        "SubmitFrame called for invalid/unregistered client index {} "
        "(in_use={}, driver={:p})",
        index, index < kMaximumClientCount ? clients_[index].in_use : false,
        index < kMaximumClientCount ? (void*)clients_[index].driver : nullptr);

    // Submit silence instead of dropping the frame to maintain the callback
    // chain.  If we don't submit anything, the audio driver's OnBufferEnd
    // callback will never fire, causing the semaphore to leak.
    if (index < kMaximumClientCount && clients_[index].driver) {
      static float silence[apu::AudioDriver::kFrameSamplesMax] = {0};
      clients_[index].frames_dropped++;
      (clients_[index].driver)->SubmitFrame(silence);
    } else if (index < kMaximumClientCount) {
      // Tick the semaphore so the worker doesn't stall on a dead client.
      client_semaphores_[index]->Release(1, nullptr);
    }
    return;
  }
  clients_[index].frames_submitted++;
  clients_[index].frames_processed++;
  if (cvars::apu_aaudio_log_stats && samples) {
    // All zero bytes in either byte order.
    const uint32_t* words = reinterpret_cast<const uint32_t*>(samples);
    uint32_t any = 0;
    for (uint32_t i = 0; i < AudioDriver::kFrameSamplesMax; ++i) {
      any |= words[i];
    }
    const bool silent = any == 0;
    if (silent) {
      clients_[index].frames_silent.fetch_add(1, std::memory_order_relaxed);
      if (!clients_[index].last_frame_silent) {
        clients_[index].silences_after_sound.fetch_add(
            1, std::memory_order_relaxed);
      }
    }
    clients_[index].last_frame_silent = silent;
  }
  (clients_[index].driver)->SubmitFrame(samples);
}

bool AudioSystem::GetClientPerformance(size_t index,
                                       ClientPerformance* out_perf) {
  if (index >= kMaximumClientCount || !out_perf) {
    return false;
  }

  std::lock_guard<std::mutex> clients_lock(clients_mutex_);
  if (!clients_[index].in_use) {
    return false;
  }

  out_perf->frames_submitted = clients_[index].frames_submitted.load();
  out_perf->frames_processed = clients_[index].frames_processed.load();
  out_perf->frames_dropped = clients_[index].frames_dropped.load();
  return true;
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  // The index comes from the guest's handle, as in SubmitFrame: one out of
  // range was written outside the clients, and a client unregistered twice had
  // its null driver destroyed (the assert is gone in release builds).
  if (index >= kMaximumClientCount) {
    XELOGE("AudioSystem::UnregisterClient: index {} out of range", index);
    return;
  }
  AudioDriver* driver_to_destroy;
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    if (!clients_[index].in_use) {
      XELOGW("AudioSystem::UnregisterClient: client {} is not registered",
             index);
      return;
    }
    XELOGI("AudioSystem::UnregisterClient: index={}, driver={:p}", index,
           (void*)clients_[index].driver);
    driver_to_destroy = clients_[index].driver;
    // Leak wrapped_callback_arg: in-flight callback may hold this pointer.
    clients_[index].driver = nullptr;
    clients_[index].callback = 0;
    clients_[index].callback_arg = 0;
    clients_[index].wrapped_callback_arg = 0;
    clients_[index].in_use = false;
    clients_[index].next_pump_us = 0;
    clients_[index].frames_submitted.store(0);
    clients_[index].frames_processed.store(0);
    clients_[index].frames_dropped.store(0);
  }

  // Wait for any in-flight callback; can't hold clients_mutex_ (callback
  // re-enters).
  {
    std::lock_guard<std::mutex> lk(clients_[index].callback_mutex);
  }

  DestroyDriver(driver_to_destroy);

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  xe::threading::WaitResult wait_result;
  do {
    wait_result = xe::threading::Wait(client_semaphore, false,
                                      std::chrono::milliseconds(0));
  } while (wait_result == xe::threading::WaitResult::kSuccess);
  assert_true(wait_result == xe::threading::WaitResult::kTimeout);
}

bool AudioSystem::Save(ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (int i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    XELOGE("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    if (id >= kMaximumClientCount) {
      XELOGE("AudioSystem::Restore - client index {} out of range", id);
      return false;
    }

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.next_pump_us = 0;
    client.last_frame_silent = true;
    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      XELOGE(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      // As in RegisterClient: the slot left unused and its credits taken back,
      // not in use without a driver.
      client.in_use = false;
      client.callback = 0;
      while (xe::threading::Wait(client_semaphore, false,
                                 std::chrono::milliseconds(0)) ==
             xe::threading::WaitResult::kSuccess) {
      }
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  pending_work_event_->Set();
  pause_fence_.Wait();

  // Only once the worker is parked: a running sink drains a queue nothing is
  // refilling, which reads as a starving producer.
  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      if (clients_[i].in_use && clients_[i].driver) {
        clients_[i].driver->Pause();
      }
    }
  }

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  {
    std::lock_guard<std::mutex> clients_lock(clients_mutex_);
    for (size_t i = 0; i < kMaximumClientCount; ++i) {
      if (clients_[i].in_use && clients_[i].driver) {
        clients_[i].driver->Resume();
      }
    }
  }

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace apu
}  // namespace xe
