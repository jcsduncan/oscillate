#pragma once

#include "oscillate/analysis/key.hpp"
#include "oscillate/analysis/tempo.hpp"
#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class SourceAnalysisService : public SessionService {
public:
  explicit SourceAnalysisService(SessionState& state) noexcept : SessionService(state) {}

  // Reviewed source analysis is durable and never silently conforms the
  // global tempo map. A worker result must match the semantic cache key and
  // the project revision captured before its scan.
  [[nodiscard]] foundation::Result<void>
  accept_source_tempo_analysis(project::SourceId source, std::uint64_t origin_revision,
                               const analysis::TempoEstimate& estimate);
  [[nodiscard]] foundation::Result<void> set_source_tempo_correction(project::SourceId source,
                                                                     std::optional<double> bpm);
  // Accept only a worker result for the exact captured revision and immutable
  // source version. Generated suggestions never replace a manual correction.
  [[nodiscard]] foundation::Result<void>
  accept_source_key_analysis(project::SourceId source, std::uint64_t origin_revision,
                             const analysis::KeyEstimate& estimate);
  [[nodiscard]] foundation::Result<void>
  set_source_key_correction(project::SourceId source,
                            std::optional<project::MusicalKey> correction);
  [[nodiscard]] foundation::Result<void>
  set_source_user_beat_frames(project::SourceId source, std::vector<std::uint64_t> frames);
  // Stores a worker's whole-source onset analysis
  // as the source's generated onset review. `analysed` and `origin_revision`
  // are what the worker captured before analysis; any intervening revision
  // or source change rejects it. Dismissed onsets and clip markers are kept.
  [[nodiscard]] foundation::Result<void>
  accept_source_onset_analysis(const project::SourceRecord& analysed, std::uint64_t origin_revision,
                               std::vector<std::uint64_t> frames, bool truncated);
  // Replaces the source's user-dismissed onsets (ascending source frames).
  [[nodiscard]] foundation::Result<void>
  set_source_dismissed_onsets(project::SourceId source, std::vector<std::uint64_t> frames);
};

} // namespace oscillate::application
