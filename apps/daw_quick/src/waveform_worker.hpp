#pragma once

#include "oscillate/analysis/waveform.hpp"
#include "oscillate/project/model.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

namespace oscillate::quick {

// One worker per window. All input and output values are owned, so a project
// switch never waits for media verification or decoder work on the UI thread.
class WaveformWorker {
public:
  using Analyzer = std::function<foundation::Result<analysis::WaveformSummary>(
      const std::filesystem::path&, const project::SourceRecord&, std::uint64_t,
      const std::function<bool()>&)>;
  struct Outcome {
    std::uint64_t generation;
    project::SourceRecord source;
    foundation::Result<analysis::WaveformSummary> result;
    bool cache_degraded = false;
  };

  explicit WaveformWorker(Analyzer analyzer = {});
  WaveformWorker(const WaveformWorker&) = delete;
  WaveformWorker& operator=(const WaveformWorker&) = delete;
  ~WaveformWorker();

  void submit(std::filesystem::path root, std::vector<project::SourceRecord> sources,
              std::uint64_t generation,
              std::optional<project::SourceId> rebuild_source = std::nullopt);
  [[nodiscard]] std::vector<Outcome> collect();

private:
  struct Batch {
    std::filesystem::path root;
    std::vector<project::SourceRecord> sources;
    std::uint64_t generation;
    std::stop_token stop;
    std::optional<project::SourceId> rebuild_source;
  };
  struct Shared {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::optional<Batch> pending;
    std::vector<Outcome> ready;
    std::atomic<std::uint64_t> generation{0};
    std::stop_source stop;
  };
  std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
  std::jthread thread_;
};

} // namespace oscillate::quick
