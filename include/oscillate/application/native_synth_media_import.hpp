#pragma once

#include "oscillate/persistence/store.hpp"
#include "oscillate/project/model.hpp"

#include <cstdint>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace oscillate::application {

class ImportService;
class PreparedNativeSynthMediaImport;

// The rest of a media import's undo step, decided from the decoded audio (a
// new sample with its tempo, root and chops' reset parameters, ADR-0063):
// the patch's sparse values and the document's labels, each replaced whole.
// Ids missing from `values` take the engine defaults.
struct NativeSynthMediaState {
  std::vector<project::NativeInstrumentParameter> values;
  std::vector<project::NativeLabel> labels;
};

// A user's audio for a built-in synth's media slot (ADR-0060, ADR-0063).
// Control-thread begin pins the input; the move-only task copies, decodes
// and hashes it on a worker without the writer lock; finish runs on the
// originating control sequencer. WAV always, and AIFF, FLAC or MP3 in the
// optional codec build.
class NativeSynthMediaImportTask {
public:
  NativeSynthMediaImportTask(NativeSynthMediaImportTask&&) noexcept = default;
  NativeSynthMediaImportTask& operator=(NativeSynthMediaImportTask&&) noexcept = default;
  NativeSynthMediaImportTask(const NativeSynthMediaImportTask&) = delete;
  NativeSynthMediaImportTask& operator=(const NativeSynthMediaImportTask&) = delete;

  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }
  [[nodiscard]] foundation::Result<PreparedNativeSynthMediaImport>
  run(const std::stop_token& stop = {}) &&;

private:
  friend class ImportService;
  NativeSynthMediaImportTask(persistence::WavStageTask stage, bool wav, std::uint64_t revision,
                             project::TrackId track, std::string slot)
      : stage_(std::move(stage)), wav_(wav), origin_revision_(revision), track_(track),
        slot_(std::move(slot)) {}

  persistence::WavStageTask stage_;
  bool wav_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  std::string slot_;
};

class PreparedNativeSynthMediaImport {
public:
  PreparedNativeSynthMediaImport(PreparedNativeSynthMediaImport&&) noexcept = default;
  PreparedNativeSynthMediaImport& operator=(PreparedNativeSynthMediaImport&&) noexcept = default;
  PreparedNativeSynthMediaImport(const PreparedNativeSynthMediaImport&) = delete;
  PreparedNativeSynthMediaImport& operator=(const PreparedNativeSynthMediaImport&) = delete;

  // The staged source: its rate, channels and frame count, for chopping
  // before finish.
  [[nodiscard]] const project::SourceRecord& source() const noexcept { return stage_.record(); }
  [[nodiscard]] std::uint64_t origin_revision() const noexcept { return origin_revision_; }
  [[nodiscard]] project::TrackId track() const noexcept { return track_; }
  [[nodiscard]] const std::string& slot() const noexcept { return slot_; }

private:
  friend class NativeSynthMediaImportTask;
  friend class ImportService;
  PreparedNativeSynthMediaImport(persistence::StagedWav stage, bool wav, std::uint64_t revision,
                                 project::TrackId track, std::string slot)
      : stage_(std::move(stage)), wav_(wav), origin_revision_(revision), track_(track),
        slot_(std::move(slot)) {}

  persistence::StagedWav stage_;
  bool wav_;
  std::uint64_t origin_revision_;
  project::TrackId track_;
  std::string slot_;
};

inline foundation::Result<PreparedNativeSynthMediaImport>
NativeSynthMediaImportTask::run(const std::stop_token& stop) && {
  auto staged = std::move(stage_).run(stop);
  if (!staged)
    return foundation::Result<PreparedNativeSynthMediaImport>::failure(staged.error());
  return foundation::Result<PreparedNativeSynthMediaImport>::success(PreparedNativeSynthMediaImport(
      std::move(staged).value(), wav_, origin_revision_, track_, std::move(slot_)));
}

} // namespace oscillate::application
