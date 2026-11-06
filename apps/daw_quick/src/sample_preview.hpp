#pragma once

#include "oscillate/application/instrument_playback.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/timeline/timing.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace oscillate::quick {

// A sample prepared for the browser's preview: decoded (at most 30 s),
// at the project's rate, and for a loop with a known tempo, stretched to
// the project's tempo with its pitch kept (ADR-0059).
struct PreparedSample {
  std::vector<float> left;
  std::vector<float> right;
  // The sample's own tempo (from its name, else its length), if a loop.
  std::optional<double> bpm;
  // True when it was stretched to the project's tempo.
  bool warped = false;
};

// Worker-only: decodes `file` and fits it to `project_rate` and, when its
// tempo is known and within 0.5x..2x of it, `project_bpm`.
[[nodiscard]] foundation::Result<PreparedSample> prepare_sample(const std::filesystem::path& file,
                                                                std::uint32_t project_rate,
                                                                std::optional<double> project_bpm,
                                                                const std::stop_token& stop = {});

// The browser's sample preview: one sample at a time on the preview device.
// play() only queues; decoding, stretching and the device run on a worker.
// A loop heard while the song plays starts on the song's next beat.
class SamplePreview final {
public:
  using ErrorSink = std::function<void(std::string)>;
  // Where the playing song is when the request was made: its tempo map and
  // rate, the playhead, and when it was read. Absent: start at once.
  struct BeatClock {
    timeline::TempoMap tempo;
    std::uint32_t sample_rate = 48'000;
    std::uint64_t playhead = 0;
    std::chrono::steady_clock::time_point read_at;
  };
  struct Request {
    std::filesystem::path file;
    std::uint32_t project_rate = 48'000;
    std::optional<double> project_bpm;
    std::optional<BeatClock> clock;
    std::uint32_t buffer_frames = 256;
    std::optional<std::uint64_t> target_serial;
    application::PlaybackOutputFactory output_factory;
  };

  explicit SamplePreview(ErrorSink error_sink = {});
  SamplePreview(const SamplePreview&) = delete;
  SamplePreview& operator=(const SamplePreview&) = delete;
  ~SamplePreview();

  [[nodiscard]] bool play(Request request);
  void stop() noexcept;

private:
  struct Stream;
  void run(std::stop_token stop_token) noexcept;
  void report(std::string message) const;

  ErrorSink error_sink_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<Request> pending_;
  bool stop_requested_ = false;
  std::jthread worker_;
};

} // namespace oscillate::quick
