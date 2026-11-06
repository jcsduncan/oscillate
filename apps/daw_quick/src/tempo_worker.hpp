#pragma once

#include "oscillate/application/tempo_cache.hpp"
#include "source_analysis_worker.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace oscillate::quick {

// A single latest-selection worker. Inputs and mailbox outlive a detached
// blocked reader, so changing projects never joins disk work on the UI thread.
class TempoWorker {
public:
  using Analyzer = std::function<foundation::Result<application::TempoCacheResult>(
      const std::filesystem::path&, const project::SourceRecord&, std::uint64_t,
      const std::function<bool()>&)>;
  using Outcome = SourceAnalysisWorker<application::TempoCacheResult>::Outcome;

  explicit TempoWorker(Analyzer analyzer = {});
  TempoWorker(const TempoWorker&) = delete;
  TempoWorker& operator=(const TempoWorker&) = delete;
  ~TempoWorker();

  void submit(std::filesystem::path root, std::optional<project::SourceRecord> source,
              std::uint64_t generation);
  [[nodiscard]] std::optional<Outcome> collect();

private:
  SourceAnalysisWorker<application::TempoCacheResult> worker_;
};

} // namespace oscillate::quick
