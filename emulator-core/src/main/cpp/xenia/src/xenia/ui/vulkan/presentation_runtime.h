#ifndef XENIA_UI_PRESENTATION_RUNTIME_H_
#define XENIA_UI_PRESENTATION_RUNTIME_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace xe::ui {
enum class DisplayMode : int { kDefault = -1, kFit = 0, kFill = 1, kStretch = 2, kInteger = 3 };
enum class FrameGenerationState : int { kOff, kWarmingUp, kRunning, kUnsupported, kFailed };

// GPU time of one generation pass per measured synthetic frame (F08): 0.25 ms
// buckets, the last one holds 16 ms and more.
constexpr size_t kGenerationGpuBuckets = 65;
constexpr int64_t kGenerationGpuBucketNs = 250000;

struct PresentationRuntime {
  std::atomic<int> display_mode{-1};
  std::atomic<bool> frame_generation_requested{false};
  std::atomic<int> frame_generation_preset{2};
  std::atomic<int> frame_generation_state{0};
  std::atomic<int> frame_generation_error{0};
  std::atomic<int> frame_generation_engine{0}; // 0 Win-FG, 1 LSFG
  std::atomic<int> frame_generation_multiplier{2};
  std::atomic<uint64_t> configuration_epoch{0};
  std::atomic<int> scaling_effect{-1}; // inherited / bilinear / CAS / FSR / SGSR / Lanczos / CRT
  // The in-game menu's Image options, applied from the next frame; a negative value
  // leaves the Settings value. Antialiasing: 0 none, 1 FXAA, 2 FXAA extreme.
  std::atomic<int> antialiasing{-1};
  std::atomic<float> cas_sharpness{-1.0f};            // CAS additional sharpness, 0..1
  std::atomic<float> fsr_sharpness_reduction{-1.0f};  // FSR sharpness reduction, 0..2 stops
  std::atomic<int> dither{-1};                        // 0 off, 1 on
  std::atomic<int> color_filter{0};
  std::atomic<int> color_filter_error{0};
  std::atomic<int> presenter_tid{0};
  std::atomic<int64_t> presenter_work_ns{0};
  std::atomic<uint64_t> presenter_work_sequence{0};
  std::mutex configuration_mutex;
  std::string lsfg_cache;
  std::string gpu_label;
  // "key=value;..." identity of the driver in use (vendor, device, driver version,
  // API, driverID/name/info, pipelineCacheUUID), set when the presenter starts.
  std::string driver_identity;
  std::atomic<float> display_hz{60.0f};
  // The guest output period the frame-generation thread measured last (its cadence), for
  // the log line when generation stops.
  std::atomic<int64_t> guest_period_ns{0};
  std::atomic<uint64_t> generated_submissions{0};
  std::atomic<uint64_t> dropped_guest_notifications{0};
  // Synthetic outputs not painted because the presenter was already past their
  // slot: they would have been presented back-to-back with the next output.
  std::atomic<uint64_t> late_synthetic_skips{0};
  // F02: synthetic output slots the schedule offered while FG was requested (multiplier-1
  // per processed guest frame). Slots = late skips + painted; of the painted ones only
  // generated_submissions became synthetic frames on screen (the rest: warm-up, fallback
  // to the real frame, a presentation that failed or a surface that was not paintable).
  std::atomic<uint64_t> synthetic_slots{0};
  // Latest generation pass, -1 when the last one could not be timed (never a stale value).
  std::atomic<double> generation_gpu_ms{-1.0};
  // Cumulative per process; the app takes deltas per run. Relaxed: counters only.
  std::array<std::atomic<uint64_t>, kGenerationGpuBuckets> generation_gpu_histogram{};
  std::atomic<uint64_t> generation_gpu_unavailable{0};
};
inline PresentationRuntime& RuntimePresentation() {
  static PresentationRuntime runtime;
  return runtime;
}

// Nanoseconds between two timestamps written on one queue. Only the low
// `valid_bits` bits are meaningful (Vulkan's timestampValidBits) and the counter
// may wrap between the two writes, so the difference is taken modulo 2^valid_bits.
// -1 when the pair is no measurement: no valid bits, no period, or an elapsed time
// no single generation pass takes (over a second: a reset or unwritten query).
inline int64_t TimestampElapsedNs(uint64_t begin, uint64_t end, uint32_t valid_bits, double period_ns) {
  if (valid_bits == 0 || !(period_ns > 0.0)) return -1;
  const uint64_t mask = valid_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << valid_bits) - 1;
  const double ns = double((end - begin) & mask) * period_ns;
  if (!(ns <= 1e9)) return -1;
  return int64_t(ns);
}

// One generation pass timed (ns >= 0) or not (ns < 0): latest value and histogram.
inline void RecordGenerationGpu(PresentationRuntime& runtime, int64_t ns) {
  if (ns < 0) {
    runtime.generation_gpu_ms.store(-1.0, std::memory_order_relaxed);
    runtime.generation_gpu_unavailable.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  runtime.generation_gpu_ms.store(double(ns) / 1e6, std::memory_order_relaxed);
  const size_t bucket = std::min<size_t>(size_t(ns / kGenerationGpuBucketNs), kGenerationGpuBuckets - 1);
  runtime.generation_gpu_histogram[bucket].fetch_add(1, std::memory_order_relaxed);
}

struct OutputRectangle { int32_t x, y; uint32_t width, height; };
inline OutputRectangle CalculateOutputRectangle(DisplayMode mode, uint32_t source_width,
    uint32_t source_height, uint32_t aspect_x, uint32_t aspect_y,
    uint32_t host_width, uint32_t host_height) {
  if (!source_width || !source_height || !aspect_x || !aspect_y || !host_width || !host_height) return {};
  if (mode == DisplayMode::kStretch) return {0, 0, host_width, host_height};
  double w = host_width;
  double h = w * aspect_y / aspect_x;
  if ((mode == DisplayMode::kFill && h < host_height) ||
      (mode != DisplayMode::kFill && h > host_height)) {
    h = host_height; w = h * aspect_x / aspect_y;
  }
  if (mode == DisplayMode::kInteger) {
    const uint32_t factor = std::min(host_width / source_width, host_height / source_height);
    if (factor) { w = double(source_width) * factor; h = double(source_height) * factor; }
  }
  const uint32_t width = std::max(1u, uint32_t(std::llround(w)));
  const uint32_t height = std::max(1u, uint32_t(std::llround(h)));
  return {int32_t((int64_t(host_width) - width) / 2),
          int32_t((int64_t(host_height) - height) / 2), width, height};
}

// Pure cadence estimator used by the worker and host tests. Never extrapolates an
// unbounded present queue; a long pause resets history rather than generating stale frames.
// The guest output period: the mean of the last kWindow intervals. Games deliver their
// frames on vblanks, so a 30 fps one alternates 16.7 and 50 ms intervals at times; the
// mean of a window is 33.3 ms, where a moving average of the last few swung below 29 ms
// and the 30 fps game was judged too fast for a 60 Hz display. A gap (pause, loading)
// starts the window over at the 30 fps default.
class FrameCadence {
 public:
  static constexpr int kWindow = 32;
  int64_t Observe(int64_t now_ns) {
    if (last_ns_ && now_ns > last_ns_) {
      const int64_t delta = now_ns - last_ns_;
      if (delta > 250000000) {
        Clear();
      } else if (delta >= 4000000) {
        sum_ns_ += delta - deltas_[next_];
        deltas_[next_] = delta;
        next_ = (next_ + 1) % kWindow;
        count_ = std::min(count_ + 1, kWindow);
        period_ns_ = sum_ns_ / count_;
      }
    }
    last_ns_ = now_ns;
    return std::clamp<int64_t>(period_ns_ / 2, 2000000, 50000000);
  }
  // Within 10%: a 30 fps game's window can sit near 32 ms on vblank-quantized frames,
  // and a little over the display rate only queues a present in FIFO. The app caps the
  // game at the display rate / multiplier when generation starts; this catches a game
  // clearly faster than that.
  bool FitsRefresh(float hz, int multiplier = 2) const {
    return hz > 0 && multiplier >= 2 && double(period_ns_) * hz >= 900000000.0 * multiplier;
  }
  // The window holds kWindow intervals: enough to judge the cadence.
  bool Full() const { return count_ == kWindow; }
  void Reset() { last_ns_ = 0; Clear(); }
  int64_t period_ns() const { return period_ns_; }
 private:
  void Clear() {
    for (int64_t& delta : deltas_) delta = 0;
    sum_ns_ = 0; next_ = 0; count_ = 0; period_ns_ = 33333333;
  }
  int64_t last_ns_ = 0;
  int64_t period_ns_ = 33333333;
  int64_t deltas_[kWindow] = {};
  int64_t sum_ns_ = 0;
  int next_ = 0;
  int count_ = 0;
};

// Decisions of the frame-generation presenter thread, free of Vulkan and of the
// real clock so the host tests can drive them. One cycle per processed source
// frame: outputs 1..multiplier-1 are synthetic, output `multiplier` is the real
// frame, spaced by period/multiplier from the moment the cycle starts. Only the
// newest notification is processed (older ones count as dropped), so a slow
// presenter never works through a backlog.
class FrameGenerationSchedule {
 public:
  struct Cycle {
    uint64_t dropped = 0;       // notifications replaced before being processed
    int multiplier = 2;         // outputs per source frame
    int64_t begin_ns = 0;
    int64_t step_ns = 0;        // spacing between consecutive outputs
    bool stop = false;          // cadence x multiplier exceeds the display rate
  };

  bool HasPending(uint64_t notification) const { return notification != consumed_; }
  int64_t period_ns() const { return cadence_.period_ns(); }

  Cycle Begin(uint64_t notification, int64_t arrival_ns, uint64_t epoch, int multiplier,
              float display_hz, int64_t now_ns) {
    Cycle cycle;
    cycle.dropped = notification > consumed_ + 1 ? notification - consumed_ - 1 : 0;
    consumed_ = notification;
    if (!has_epoch_ || epoch != epoch_) {
      // New configuration (engine, multiplier, Hz): judge the cadence afresh.
      epoch_ = epoch;
      has_epoch_ = true;
      cadence_.Reset();
    }
    const int64_t half_period = cadence_.Observe(arrival_ns);
    cycle.multiplier = std::clamp(multiplier, 2, 4);
    // Judged once the window is full, so never on the burst after a pause either.
    cycle.stop = cadence_.Full() && !cadence_.FitsRefresh(display_hz, cycle.multiplier);
    cycle.begin_ns = now_ns;
    cycle.step_ns = half_period * 2 / cycle.multiplier;
    return cycle;
  }

  // When output `index` (1-based; `multiplier` is the real frame) is due: counted
  // from the real frame that STARTED this cycle (begin_ns, already in the past by the
  // time Begin() returns), so index 1 lands step_ns AFTER it, not at it. With
  // multiplier 2 (ZeroFG, Win-FG) that one synthetic output sits exactly halfway to
  // the next real frame: 15 ms after a real frame if the last two real frames were
  // 30 ms apart. index == multiplier lands exactly one full real-frame gap later,
  // which is also where the following cycle naturally begins.
  static int64_t Deadline(const Cycle& cycle, int index) {
    return cycle.begin_ns + cycle.step_ns * index;
  }

  // A synthetic output whose slot passed by more than half a step is skipped
  // instead of being presented right before the next one (no catch-up burst).
  // The real frame is never skipped.
  static bool LateSynthetic(const Cycle& cycle, int index, int64_t now_ns) {
    return index < cycle.multiplier && now_ns > Deadline(cycle, index) + cycle.step_ns / 2;
  }

 private:
  FrameCadence cadence_;
  uint64_t consumed_ = 0;
  uint64_t epoch_ = 0;
  bool has_epoch_ = false;
};

inline int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Shared by the producer (a guest output arriving) and the frame-generation thread.
struct FrameGenerationQueue {
  std::mutex mutex;
  std::condition_variable condition;
  bool shutdown = false;
  uint64_t notification = 0;  // guest outputs announced so far
  int64_t arrival_ns = 0;     // steady clock of the newest one

  // A new guest output; one the thread has not processed yet is replaced, not queued.
  void Notify(int64_t now_ns) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++notification;
      arrival_ns = now_ns;
    }
    condition.notify_one();
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      shutdown = true;
    }
    condition.notify_all();
  }
};

// The frame-generation thread (A02), free of Vulkan so the host tests run the real loop
// with a fake painter: one cycle per processed guest output (only the newest; older ones
// count as dropped), synthetic outputs 1..multiplier-1 at their deadlines (a late one is
// skipped, never presented back-to-back), then the real output, phase 0. Lock order: the
// queue's mutex is never held while painting (painting takes the presenter's paint mode
// mutex), so the producer can never wait on a paint. After shutdown is seen nothing more
// is painted; a paint already running finishes first.
template <typename Paint>
void RunFrameGenerationLoop(FrameGenerationQueue& queue, PresentationRuntime& runtime, Paint&& paint) {
  FrameGenerationSchedule schedule;
  std::unique_lock<std::mutex> lock(queue.mutex);
  while (!queue.shutdown) {
    queue.condition.wait(lock, [&] { return queue.shutdown || schedule.HasPending(queue.notification); });
    if (queue.shutdown) break;
    const int engine_multiplier = runtime.frame_generation_engine.load() == 1
        ? runtime.frame_generation_multiplier.load() : 2;
    const FrameGenerationSchedule::Cycle cycle = schedule.Begin(
        queue.notification, queue.arrival_ns, runtime.configuration_epoch.load(), engine_multiplier,
        runtime.display_hz.load(), SteadyNowNs());
    if (cycle.dropped) runtime.dropped_guest_notifications.fetch_add(cycle.dropped);
    runtime.guest_period_ns.store(schedule.period_ns(), std::memory_order_relaxed);
    if (cycle.stop) {
      runtime.frame_generation_error = 5;
      runtime.frame_generation_state = int(FrameGenerationState::kFailed);
      runtime.frame_generation_requested = false;
    }
    lock.unlock();
    bool shutdown = false;
    for (int index = 1; index < cycle.multiplier && !shutdown; ++index) {
      if (runtime.frame_generation_requested.load()) {
        runtime.synthetic_slots.fetch_add(1, std::memory_order_relaxed);
        if (FrameGenerationSchedule::LateSynthetic(cycle, index, SteadyNowNs())) {
          runtime.late_synthetic_skips.fetch_add(1, std::memory_order_relaxed);
        } else {
          paint(index);
        }
      }
      lock.lock();
      // At most one cycle is in flight; a newer notification waits for the next cycle
      // and replaces any older pending one.
      queue.condition.wait_until(
          lock,
          std::chrono::steady_clock::time_point(
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::nanoseconds(FrameGenerationSchedule::Deadline(cycle, index + 1)))),
          [&] { return queue.shutdown; });
      shutdown = queue.shutdown;
      lock.unlock();
    }
    if (shutdown) break;
    paint(0);
    lock.lock();
  }
}
}  // namespace xe::ui
#endif
