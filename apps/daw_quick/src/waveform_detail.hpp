#pragma once

#include "oscillate/media/prefetch.hpp"
#include "oscillate/project/model.hpp"

#include <QVariantList>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace oscillate::quick {

// The source's own frames over a stretch of it, for drawing a clip zoomed in
// past the summary's finest level: every column then shows the exact samples
// it covers, down to single frames, so kicks keep their shape at any zoom.
struct WaveformDetail {
  std::uint64_t first_frame = 0;
  std::vector<media::StereoFrame> frames;

  [[nodiscard]] std::uint64_t end_frame() const noexcept { return first_frame + frames.size(); }
  [[nodiscard]] bool covers(std::uint64_t begin, std::uint64_t end) const noexcept {
    return !frames.empty() && begin >= first_frame && end <= end_frame();
  }
};

// One {min, max} envelope per column between consecutive `edges` (absolute
// source frames, ascending), from the detail's own frames. A column narrower
// than one frame shows the frame it plays. Empty unless the detail covers
// every edge.
[[nodiscard]] QVariantList detail_columns(const WaveformDetail& detail,
                                          std::span<const std::uint64_t> edges);

// One worker per window. It keeps the latest wanted stretch per source and
// reads it with a reader it holds open, so panning close-up re-reads only
// what moved into view. Results are collected on the UI thread.
class WaveformDetailWorker {
public:
  // The most frames one request may read: three screens at the finest
  // summary level, a few seconds of audio.
  static constexpr std::uint64_t max_frames = 4'194'304;

  struct Outcome {
    std::uint64_t generation;
    std::string source_id;
    WaveformDetail detail;
  };

  WaveformDetailWorker();
  WaveformDetailWorker(const WaveformDetailWorker&) = delete;
  WaveformDetailWorker& operator=(const WaveformDetailWorker&) = delete;
  ~WaveformDetailWorker();

  void request(std::filesystem::path root, project::SourceRecord source, std::uint64_t begin,
               std::uint64_t end, std::uint64_t generation);
  [[nodiscard]] std::vector<Outcome> collect();

private:
  struct Request {
    std::filesystem::path root;
    project::SourceRecord source;
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t generation;
  };
  struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unordered_map<std::string, Request> pending;
    std::vector<Outcome> ready;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
};

} // namespace oscillate::quick
