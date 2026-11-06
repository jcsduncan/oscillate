#pragma once

#include "oscillate/persistence/store.hpp"

#include <cstdint>
#include <stop_token>
#include <utility>

namespace oscillate::application {

class ImportService;
class PreparedWavAudioImport;

// Created on the project control thread, then moved to one worker. The task
// owns only pinned descriptors and a private staging file, never the writer.
class WavAudioImportTask {
public:
  WavAudioImportTask(WavAudioImportTask&&) noexcept = default;
  WavAudioImportTask& operator=(WavAudioImportTask&&) noexcept = default;
  WavAudioImportTask(const WavAudioImportTask&) = delete;
  WavAudioImportTask& operator=(const WavAudioImportTask&) = delete;

  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }
  [[nodiscard]] foundation::Result<PreparedWavAudioImport> run(const std::stop_token& stop = {}) &&;

private:
  friend class ImportService;
  WavAudioImportTask(persistence::WavStageTask stage, std::uint64_t revision,
                     project::TrackId track, project::ClipId clip)
      : stage_(std::move(stage)), origin_revision_(revision), track_(track), clip_(clip) {}

  persistence::WavStageTask stage_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  project::ClipId clip_;
};

// Hand back to the same project control sequencer for final validation,
// publication and one undoable project revision. Dropping it cancels import.
class PreparedWavAudioImport {
public:
  PreparedWavAudioImport(PreparedWavAudioImport&&) noexcept = default;
  PreparedWavAudioImport& operator=(PreparedWavAudioImport&&) noexcept = default;
  PreparedWavAudioImport(const PreparedWavAudioImport&) = delete;
  PreparedWavAudioImport& operator=(const PreparedWavAudioImport&) = delete;

  [[nodiscard]] const project::SourceRecord& source() const noexcept { return stage_.record(); }
  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }

private:
  friend class WavAudioImportTask;
  friend class ImportService;
  PreparedWavAudioImport(persistence::StagedWav stage, std::uint64_t revision,
                         project::TrackId track, project::ClipId clip)
      : stage_(std::move(stage)), origin_revision_(revision), track_(track), clip_(clip) {}

  persistence::StagedWav stage_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  project::ClipId clip_;
};

} // namespace oscillate::application
