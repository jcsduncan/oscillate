#pragma once

#include "oscillate/application/encoded_audio_import.hpp"
#include "oscillate/application/external_source_relink.hpp"
#include "oscillate/application/midi_file_import.hpp"
#include "oscillate/application/native_synth_media_import.hpp"
#include "oscillate/application/session/save_service.hpp"
#include "oscillate/application/session/session_service.hpp"
#include "oscillate/application/wav_audio_import.hpp"

namespace oscillate::application {

class ImportService : public SessionService {
public:
  ImportService(SessionState& state, SaveService& saving) noexcept
      : SessionService(state), saving_(saving) {}

  // A user's audio in a synth's media slot (a kick's transient, a sample to
  // chop): the file read from input_fd (WAV, or AIFF, FLAC or MP3 in codec
  // builds; any rate, mono or stereo, at most
  // project::max_native_synth_media_frames) becomes a managed source and the
  // slot `slot` of the track's document plays it, in one undo step, then
  // saves. With `regions`, they replace the document's regions in that slot
  // in the same step, and with `state` the patch's values and labels (a
  // sample with its first chops, tempo and root). The engine resolves the
  // new patch before anything changes. Begin is control-thread only and leaves the model unchanged;
  // run the task on a worker, then finish on the same control sequencer. A stale completion is
  // rejected before the asset is published.
  [[nodiscard]] foundation::Result<NativeSynthMediaImportTask>
  begin_native_synth_media_import(project::TrackId track, std::string_view slot, int input_fd);
  [[nodiscard]] foundation::Result<void> finish_native_synth_media_import(
      PreparedNativeSynthMediaImport&& prepared,
      std::optional<std::vector<project::NativeRegion>> regions = std::nullopt,
      std::optional<NativeSynthMediaState> state = std::nullopt);
  // Begin, run and finish in one call, for short or bundled sounds.
  [[nodiscard]] foundation::Result<void> import_native_synth_media(
      project::TrackId track, std::string_view slot, int input_fd,
      std::optional<std::vector<project::NativeRegion>> regions = std::nullopt);
  // Stages a same-rate pinned WAV descriptor, adds one audio track and clip at
  // timeline zero in one undo step, then saves. An accepted edit and receipt
  // remain in memory for save() retry if persistence fails.
  // Imports a Standard MIDI File (ADR-0045) as one undo step. The descriptor
  // is read to its end, at most midi_file::max_file_bytes.
  [[nodiscard]] foundation::Result<MidiImportResult>
  import_midi_file(int input_fd, const MidiImportOptions& options = {});
  [[nodiscard]] foundation::Result<MidiImportResult>
  import_midi_bytes(std::span<const std::uint8_t> bytes, const MidiImportOptions& options = {});
  [[nodiscard]] foundation::Result<project::TrackId> import_wav_audio(int input_fd);
  // Begin is control-thread only and leaves the model unchanged. Run the
  // returned task on a worker, then finish on the same serialized control
  // thread. A stale completion is rejected before an asset is published.
  [[nodiscard]] foundation::Result<WavAudioImportTask> begin_wav_audio_import(int input_fd);
  [[nodiscard]] foundation::Result<project::TrackId>
  finish_wav_audio_import(PreparedWavAudioImport&& prepared,
                          std::optional<std::size_t> insert_index = std::nullopt,
                          std::string clip_name = {});
  // Optional codec build: stages original AIFF, FLAC or MP3 bytes on a worker.
  // The same-rate result commits only on the originating project sequencer.
  // Codec-off builds return unsupported at begin, before any asset is staged.
  [[nodiscard]] foundation::Result<EncodedAudioImportTask> begin_encoded_audio_import(int input_fd);
  [[nodiscard]] foundation::Result<project::TrackId>
  finish_encoded_audio_import(PreparedEncodedAudioImport&& prepared,
                              std::optional<std::size_t> insert_index = std::nullopt,
                              std::string clip_name = {});
  // Capture on control, verify full bytes and decoded metadata on a worker,
  // then finish on the same control sequencer. A stale completion changes
  // neither model nor CURRENT. A failed save retains the accepted model edit.
  [[nodiscard]] foundation::Result<ExternalSourceRelinkTask>
  begin_external_source_relink(project::SourceId source, std::string replacement_path);
  [[nodiscard]] foundation::Result<void>
  finish_external_source_relink(PreparedExternalSourceRelink&& prepared);

private:
  [[nodiscard]] foundation::Result<project::TrackId>
  finish_audio_import(persistence::StagedWav&& stage, std::uint64_t origin_revision,
                      project::TrackId track, project::ClipId clip, bool encoded,
                      std::optional<std::size_t> insert_index, std::string clip_name);
  SaveService& saving_;
};

} // namespace oscillate::application
