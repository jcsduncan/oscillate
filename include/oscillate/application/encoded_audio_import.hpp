#pragma once

#include "oscillate/persistence/store.hpp"

#include <cstdint>
#include <stop_token>
#include <utility>

namespace oscillate::application {

class ImportService;
class PreparedEncodedAudioImport;

// Control-thread preparation pins the input. The move-only task performs
// decoding/validation and private staging on a worker without the writer lock.
class EncodedAudioImportTask {
public:
  EncodedAudioImportTask(EncodedAudioImportTask&&) noexcept = default;
  EncodedAudioImportTask& operator=(EncodedAudioImportTask&&) noexcept = default;
  EncodedAudioImportTask(const EncodedAudioImportTask&) = delete;
  EncodedAudioImportTask& operator=(const EncodedAudioImportTask&) = delete;

  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }
  [[nodiscard]] foundation::Result<PreparedEncodedAudioImport>
  run(const std::stop_token& stop = {}) &&;

private:
  friend class ImportService;
  EncodedAudioImportTask(persistence::EncodedStageTask stage, std::uint64_t revision,
                         project::TrackId track, project::ClipId clip)
      : stage_(std::move(stage)), origin_revision_(revision), track_(track), clip_(clip) {}

  persistence::EncodedStageTask stage_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  project::ClipId clip_;
};

class PreparedEncodedAudioImport {
public:
  PreparedEncodedAudioImport(PreparedEncodedAudioImport&&) noexcept = default;
  PreparedEncodedAudioImport& operator=(PreparedEncodedAudioImport&&) noexcept = default;
  PreparedEncodedAudioImport(const PreparedEncodedAudioImport&) = delete;
  PreparedEncodedAudioImport& operator=(const PreparedEncodedAudioImport&) = delete;

  [[nodiscard]] const project::SourceRecord& source() const noexcept { return stage_.record(); }
  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }

private:
  friend class EncodedAudioImportTask;
  friend class ImportService;
  PreparedEncodedAudioImport(persistence::StagedEncoded stage, std::uint64_t revision,
                             project::TrackId track, project::ClipId clip)
      : stage_(std::move(stage)), origin_revision_(revision), track_(track), clip_(clip) {}

  persistence::StagedEncoded stage_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  project::ClipId clip_;
};

inline foundation::Result<PreparedEncodedAudioImport>
EncodedAudioImportTask::run(const std::stop_token& stop) && {
  auto staged = std::move(stage_).run(stop);
  if (!staged)
    return foundation::Result<PreparedEncodedAudioImport>::failure(staged.error());
  return foundation::Result<PreparedEncodedAudioImport>::success(
      PreparedEncodedAudioImport(std::move(staged).value(), origin_revision_, track_, clip_));
}

} // namespace oscillate::application
