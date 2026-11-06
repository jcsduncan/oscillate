#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace oscillate::audio_io {

// Counts callback wall times as fractions of that callback's nominal period.
// A control thread may subtract bins from two snapshots only when epoch, rate,
// and frames match. Bin upper bounds are 25, 50, 75, 90, 100, 125, and 150%;
// the last bin contains values above 150%. Percentiles are therefore bounded
// estimates, not exact durations or backend xrun counts.
struct CallbackTimingDistribution {
  static constexpr std::size_t bin_count = 8;
  std::uint64_t epoch = 0;
  std::uint32_t rate = 0;
  std::uint32_t frames = 0;
  std::array<std::uint64_t, bin_count> bins{};
};

// Historical observation only. The free-time bound uses the upper edge of the
// slowest populated bin in a single format epoch; it is not a future deadline
// guarantee or an xrun count. The >100% bins yield zero observed free time.
struct CallbackTimingWindow {
  std::uint64_t blocks = 0;
  std::uint32_t slowest_bin_upper_percent = 0;
  std::uint32_t observed_free_us_lower_bound = 0;
};

// The device callback's time as a fraction of its block's real-time
// duration: 1.0 means a block took as long to render as it lasts. Measured
// over the whole callback (dequeue to queue), so it includes metering and
// the wait for audio workers. This is the one load figure the UI meter and
// the trace's audio.load counter show.
struct CallbackLoad {
  // Exponential average with a time constant of average_seconds.
  double average = 0.0;
  // The highest block load, decaying by e every peak_release_seconds.
  double peak = 0.0;
  static constexpr double average_seconds = 0.5;
  static constexpr double peak_release_seconds = 1.5;
};

[[nodiscard]] std::optional<CallbackTimingWindow>
summarize_callback_window(const CallbackTimingDistribution& before,
                          const CallbackTimingDistribution& after,
                          std::uint64_t minimum_blocks = 256) noexcept;

class CallbackHeadroomTelemetry {
public:
  struct Sample {
    std::uint64_t elapsed_ns;
    std::uint32_t frames;
    std::uint32_t rate;
  };

  // reset() is a control-thread operation, allowed only after the callback
  // has stopped. record() has one callback-thread writer and never allocates.
  void reset() noexcept;
  void record(Sample sample) noexcept;
  [[nodiscard]] std::optional<CallbackTimingDistribution> snapshot() const noexcept;

private:
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<std::uint32_t> rate_{0};
  std::atomic<std::uint32_t> frames_{0};
  std::array<std::atomic<std::uint64_t>, CallbackTimingDistribution::bin_count> bins_{};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

} // namespace oscillate::audio_io
