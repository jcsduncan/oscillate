#include "oscillate/application/session/import_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/media/source_rate_reader.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
using foundation::Result;

// RIFF/WAVE by its header, read without moving the descriptor's offset;
// anything else goes to the codec stage (unsupported in codec-off builds).
[[nodiscard]] bool wav_header(int input_fd) {
  std::array<char, 12> header{};
  const auto read = ::pread(input_fd, header.data(), header.size(), 0);
  return read == static_cast<ssize_t>(header.size()) &&
         std::string_view(header.data(), 4) == "RIFF" &&
         std::string_view(header.data() + 8, 4) == "WAVE";
}

[[nodiscard]] const project::NativeInstrumentPatch* native_patch(const project::Snapshot& state,
                                                                 project::TrackId track) {
  const auto owner = std::ranges::find(state.tracks, track, &project::Track::id);
  return owner == state.tracks.end()
             ? nullptr
             : std::get_if<project::NativeInstrumentPatch>(&owner->instrument_source);
}

} // namespace

foundation::Result<MidiImportResult>
ImportService::import_midi_file(int input_fd, const MidiImportOptions& options) {
  using Result = foundation::Result<MidiImportResult>;
  if (input_fd < 0)
    return Result::failure({foundation::ErrorCode::invalid_argument, "Invalid MIDI file"});
  // Read one byte past the limit so an oversized file is refused, not cut.
  std::vector<std::uint8_t> bytes;
  std::array<std::uint8_t, 65'536> buffer{};
  while (bytes.size() <= midi_file::max_file_bytes) {
    const auto count = ::read(input_fd, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return Result::failure({foundation::ErrorCode::io_failure, "Cannot read the MIDI file"});
    if (count == 0)
      return import_midi_bytes(bytes, options);
    bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
  }
  return Result::failure(
      {foundation::ErrorCode::resource_exhausted, "MIDI file is larger than 32 MiB"});
}

foundation::Result<MidiImportResult>
ImportService::import_midi_bytes(std::span<const std::uint8_t> bytes,
                                 const MidiImportOptions& options) {
  using Result = foundation::Result<MidiImportResult>;
  auto file = midi_file::parse(bytes);
  if (!file)
    return Result::failure(file.error());
  auto plan = plan_midi_import(snapshot(), file.value(), options,
                               {next_track_number(), next_clip_number(), next_note_number()});
  if (!plan)
    return Result::failure(plan.error());
  auto applied = model().apply(plan.value().operations);
  if (!applied)
    return Result::failure(applied.error());
  return Result::success(std::move(plan.value().result));
}

foundation::Result<project::TrackId> ImportService::import_wav_audio(int input_fd) {
  auto task = begin_wav_audio_import(input_fd);
  if (!task)
    return foundation::Result<project::TrackId>::failure(task.error());
  auto staged = std::move(task).value().run();
  if (!staged)
    return foundation::Result<project::TrackId>::failure(staged.error());
  return finish_wav_audio_import(std::move(staged).value());
}

foundation::Result<WavAudioImportTask> ImportService::begin_wav_audio_import(int input_fd) {
  if (state_.repair_state)
    return foundation::Result<WavAudioImportTask>::failure(repair_in_progress());
  auto track_id = numbered_id<project::TrackId>(next_track_number());
  auto clip_id = numbered_id<project::ClipId>(next_clip_number());
  auto source_id = numbered_id<project::SourceId>(next_source_number());
  if (!track_id) {
    return foundation::Result<WavAudioImportTask>::failure(track_id.error());
  }
  if (!clip_id) {
    return foundation::Result<WavAudioImportTask>::failure(clip_id.error());
  }
  if (!source_id) {
    return foundation::Result<WavAudioImportTask>::failure(source_id.error());
  }
  auto stage = state_.store.begin_wav_stage(source_id.value(), input_fd);
  if (!stage)
    return foundation::Result<WavAudioImportTask>::failure(stage.error());
  return foundation::Result<WavAudioImportTask>::success(WavAudioImportTask(
      std::move(stage).value(), snapshot().revision, track_id.value(), clip_id.value()));
}

foundation::Result<project::TrackId>
ImportService::finish_wav_audio_import(PreparedWavAudioImport&& prepared,
                                       std::optional<std::size_t> insert_index,
                                       std::string clip_name) {
  return finish_audio_import(std::move(prepared.stage_), prepared.origin_revision_, prepared.track_,
                             prepared.clip_, false, insert_index, std::move(clip_name));
}

foundation::Result<EncodedAudioImportTask> ImportService::begin_encoded_audio_import(int input_fd) {
  if (state_.repair_state)
    return foundation::Result<EncodedAudioImportTask>::failure(repair_in_progress());
  auto track_id = numbered_id<project::TrackId>(next_track_number());
  auto clip_id = numbered_id<project::ClipId>(next_clip_number());
  auto source_id = numbered_id<project::SourceId>(next_source_number());
  if (!track_id)
    return foundation::Result<EncodedAudioImportTask>::failure(track_id.error());
  if (!clip_id)
    return foundation::Result<EncodedAudioImportTask>::failure(clip_id.error());
  if (!source_id)
    return foundation::Result<EncodedAudioImportTask>::failure(source_id.error());
  auto stage = state_.store.begin_encoded_stage(source_id.value(), input_fd);
  if (!stage)
    return foundation::Result<EncodedAudioImportTask>::failure(stage.error());
  return foundation::Result<EncodedAudioImportTask>::success(EncodedAudioImportTask(
      std::move(stage).value(), snapshot().revision, track_id.value(), clip_id.value()));
}

foundation::Result<project::TrackId>
ImportService::finish_encoded_audio_import(PreparedEncodedAudioImport&& prepared,
                                           std::optional<std::size_t> insert_index,
                                           std::string clip_name) {
  return finish_audio_import(std::move(prepared.stage_), prepared.origin_revision_, prepared.track_,
                             prepared.clip_, true, insert_index, std::move(clip_name));
}

foundation::Result<ExternalSourceRelinkTask>
ImportService::begin_external_source_relink(project::SourceId id, std::string replacement_path) {
  using Result = foundation::Result<ExternalSourceRelinkTask>;
  if (state_.repair_state)
    return Result::failure(repair_in_progress());
  const auto found = std::ranges::find(snapshot().sources, id, &project::SourceRecord::id);
  if (found == snapshot().sources.end())
    return Result::failure({foundation::ErrorCode::not_found, "Source to relink not found"});
  if (!found->external_link)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Managed source cannot be relinked externally"});
  const bool same_path = replacement_path == found->external_link->last_known_path;
  if (snapshot().revision == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Project revision space exhausted"});
  auto candidate = *found;
  candidate.external_link = project::ExternalSourceLink{std::move(replacement_path)};
  const auto candidate_revision = snapshot().revision + (same_path ? 0U : 1U);
  auto verification =
      state_.store.begin_external_source_verification(candidate, candidate_revision);
  if (!verification)
    return Result::failure(verification.error());
  return Result::success(ExternalSourceRelinkTask(std::move(verification).value(), *found,
                                                  snapshot().revision, same_path));
}

foundation::Result<void>
ImportService::finish_external_source_relink(PreparedExternalSourceRelink&& prepared) {
  using Result = foundation::Result<void>;
  if (state_.repair_state)
    return Result::failure(repair_in_progress());
  auto stopped = state_.gate->acquire_stopped();
  if (!stopped)
    return Result::failure(stopped.error());
  const auto found =
      std::ranges::find(snapshot().sources, prepared.original_.id, &project::SourceRecord::id);
  if (snapshot().revision != prepared.origin_revision_ || found == snapshot().sources.end() ||
      *found != prepared.original_)
    return Result::failure(
        {foundation::ErrorCode::conflict, "External relink completed for an older project state"});
  if (!prepared.original_.external_link || !prepared.proof_.record().external_link)
    return Result::failure(
        {foundation::ErrorCode::corrupt_data, "External relink proof has no linked path"});
  if (!prepared.same_path_) {
    auto applied = model().apply(
        {project::RelinkSource{prepared.origin_revision_, prepared.original_.id,
                               prepared.original_.external_link->last_known_path,
                               prepared.proof_.record().external_link->last_known_path}});
    if (!applied)
      return Result::failure(applied.error());
  }
  state_.pending_external_proof.emplace(std::move(prepared.proof_));
  auto saved = saving_.save();
  if (!saved)
    return Result::failure(
        {saved.error().code,
         "External source verified in current project, but save failed: " + saved.error().message});
  return Result::success();
}

foundation::Result<project::TrackId>
ImportService::finish_audio_import(persistence::StagedWav&& stage, std::uint64_t origin_revision,
                                   project::TrackId track, project::ClipId clip_id, bool encoded,
                                   std::optional<std::size_t> insert_index, std::string clip_name) {
  if (state_.repair_state)
    return foundation::Result<project::TrackId>::failure(repair_in_progress());
  if (origin_revision != snapshot().revision || track.bits().low != next_track_number() ||
      clip_id.bits().low != next_clip_number() ||
      stage.record().id.bits().low != next_source_number()) {
    return foundation::Result<project::TrackId>::failure(
        {foundation::ErrorCode::conflict,
         "Audio import is stale; retry against the current project"});
  }
  const auto& record = stage.record();
  auto supported = media::source_rate_supported(record.sample_rate, snapshot().sample_rate);
  if (!supported)
    return foundation::Result<project::TrackId>::failure(supported.error());
  auto placement_frames =
      media::source_rate_frame_count(record.frames, record.sample_rate, snapshot().sample_rate);
  if (!placement_frames)
    return foundation::Result<project::TrackId>::failure(placement_frames.error());
  if (record.frames == 0 || placement_frames.value() == 0 ||
      record.frames > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      placement_frames.value() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return foundation::Result<project::TrackId>::failure(
        {foundation::ErrorCode::unsupported, "Audio duration exceeds project timeline limits"});
  }
  const auto existing = std::ranges::find_if(snapshot().sources, [&record](const auto& source) {
    return source.sha256 == record.sha256 && source.byte_length == record.byte_length &&
           source.encoding == record.encoding && source.sample_rate == record.sample_rate &&
           source.channels == record.channels && source.frames == record.frames;
  });
  const bool is_new_source = existing == snapshot().sources.end();
  const auto clip_source = is_new_source ? record.id : existing->id;
  const auto source_end = static_cast<std::int64_t>(record.frames);
  const auto placement_end = static_cast<std::int64_t>(placement_frames.value());
  project::Clip clip{
      clip_id, clip_source, {{0}, {source_end}}, project::ElapsedInterval{{0}, {placement_end}}, 1};
  clip.name = presentable_name(clip_name);
  std::vector<project::Operation> operations;
  if (is_new_source) {
    state_.pending_receipts.reserve(state_.pending_receipts.size() + 1);
    operations.emplace_back(project::AddSource{record});
  }
  operations.emplace_back(project::AddTrack{track, project::TrackKind::audio, insert_index});
  operations.emplace_back(project::AddClip{track, clip});
  // Preflight the complete command before making any managed asset visible.
  auto candidate = project::ProjectModel::open(snapshot(), *state_.gate);
  if (!candidate)
    return foundation::Result<project::TrackId>::failure(candidate.error());
  auto validated = candidate.value().apply(operations);
  if (!validated)
    return foundation::Result<project::TrackId>::failure(validated.error());
  // Even a duplicate source record may need its missing asset restored. The
  // worker verified any existing on-disk asset before this bounded handoff.
  auto published = encoded ? state_.store.publish_encoded_stage(std::move(stage))
                           : state_.store.publish_wav_stage(std::move(stage));
  if (!published)
    return foundation::Result<project::TrackId>::failure(published.error());
  auto applied = model().apply(operations);
  if (!applied) {
    return foundation::Result<project::TrackId>::failure(applied.error());
  }
  if (is_new_source) {
    state_.pending_receipts.push_back(std::move(published).value());
  }
  auto saved = saving_.save();
  if (!saved) {
    return foundation::Result<project::TrackId>::failure(
        {saved.error().code,
         "Audio imported in current project, but save failed: " + saved.error().message});
  }
  return foundation::Result<project::TrackId>::success(track);
}

Result<NativeSynthMediaImportTask>
ImportService::begin_native_synth_media_import(project::TrackId track, std::string_view slot,
                                               int input_fd) {
  using R = Result<NativeSynthMediaImportTask>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  if (native_patch(snapshot(), track) == nullptr)
    return R::failure({ErrorCode::unavailable, "Track has no built-in synth"});
  if (input_fd < 0)
    return R::failure({ErrorCode::invalid_argument, "Choose an audio file"});
  const auto number = next_source_number();
  if (number == 0)
    return R::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  auto source_id = project::SourceId::from_bits({0, number});
  if (!source_id)
    return R::failure(source_id.error());
  const bool wav = wav_header(input_fd);
  auto stage = wav ? state_.store.begin_wav_stage(source_id.value(), input_fd)
                   : state_.store.begin_encoded_stage(source_id.value(), input_fd);
  if (!stage)
    return R::failure(stage.error());
  return R::success(NativeSynthMediaImportTask(std::move(stage).value(), wav, snapshot().revision,
                                               track, std::string(slot)));
}

Result<void> ImportService::finish_native_synth_media_import(
    PreparedNativeSynthMediaImport&& prepared,
    std::optional<std::vector<project::NativeRegion>> regions,
    std::optional<NativeSynthMediaState> state) {
  if (state_.repair_state)
    return Result<void>::failure(repair_in_progress());
  const auto& current = snapshot();
  const auto& record = prepared.stage_.record();
  if (prepared.origin_revision_ != current.revision || record.id.bits().low != next_source_number())
    return Result<void>::failure(
        {ErrorCode::conflict, "Sound import is stale; retry against the current project"});
  const auto* patch = native_patch(current, prepared.track_);
  if (patch == nullptr)
    return Result<void>::failure({ErrorCode::unavailable, "Track has no built-in synth"});
  if (record.frames == 0 || record.frames > project::max_native_synth_media_frames ||
      record.channels == 0 || record.channels > 2)
    return Result<void>::failure(
        {ErrorCode::invalid_argument, "Use a mono or stereo sound of at most " +
                                          std::to_string(project::max_native_synth_media_frames) +
                                          " frames"});
  const auto existing = std::ranges::find_if(current.sources, [&record](const auto& source) {
    return source.sha256 == record.sha256 && source.byte_length == record.byte_length &&
           source.encoding == record.encoding && source.sample_rate == record.sample_rate &&
           source.channels == record.channels && source.frames == record.frames &&
           !source.external_link;
  });
  const bool is_new_source = existing == current.sources.end();
  const auto& source = is_new_source ? record : *existing;
  auto next = *patch;
  const project::ManagedAudioReference reference{source.id,          source.sha256,
                                                 source.byte_length, source.sample_rate,
                                                 source.channels,    source.frames};
  const std::string_view slot = prepared.slot_;
  auto& media = next.document.media;
  const auto found = std::ranges::find(media, slot, &project::NativeMediaSlot::id);
  if (found != media.end()) {
    found->media = reference;
    found->frame_length = 0;
  } else {
    media.push_back({std::string(slot), reference, 0});
  }
  if (regions) {
    std::erase_if(next.document.regions,
                  [&](const project::NativeRegion& region) { return region.media == slot; });
    next.document.regions.insert(next.document.regions.end(), regions->begin(), regions->end());
  }
  if (state) {
    next.parameters = std::move(state->values);
    next.document.labels = std::move(state->labels);
  }
  auto resolved = native_dsp::synth::resolve_native_instrument(next);
  if (!resolved)
    return Result<void>::failure(resolved.error());
  if (state) {
    // Stored in canonical form: only non-default values.
    auto canonical = native_dsp::synth::make_native_patch(
        *resolved.value().engine, resolved.value().values, std::move(next.document));
    if (!canonical)
      return Result<void>::failure(canonical.error());
    next = std::move(canonical).value();
  }
  std::vector<project::Operation> operations;
  if (is_new_source) {
    state_.pending_receipts.reserve(state_.pending_receipts.size() + 1);
    operations.emplace_back(project::AddSource{record});
  }
  operations.emplace_back(project::SetNativeInstrumentPatch{prepared.track_, std::move(next)});
  // Preflight the whole command before the managed asset becomes visible.
  auto candidate = project::ProjectModel::open(current, *state_.gate);
  if (!candidate)
    return Result<void>::failure(candidate.error());
  if (auto validated = candidate.value().apply(operations); !validated)
    return Result<void>::failure(validated.error());
  auto published = prepared.wav_ ? state_.store.publish_wav_stage(std::move(prepared.stage_))
                                 : state_.store.publish_encoded_stage(std::move(prepared.stage_));
  if (!published)
    return Result<void>::failure(published.error());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  if (is_new_source)
    state_.pending_receipts.push_back(std::move(published).value());
  auto saved = saving_.save();
  if (!saved)
    return Result<void>::failure(
        {saved.error().code,
         "Sound added to the current project, but save failed: " + saved.error().message});
  return Result<void>::success();
}

Result<void> ImportService::import_native_synth_media(
    project::TrackId track, std::string_view slot, int input_fd,
    std::optional<std::vector<project::NativeRegion>> regions) {
  auto task = begin_native_synth_media_import(track, slot, input_fd);
  if (!task)
    return Result<void>::failure(task.error());
  // A bundled or short sound: staging it here is a brief read and hash.
  auto prepared = std::move(task).value().run();
  if (!prepared)
    return Result<void>::failure(prepared.error());
  return finish_native_synth_media_import(std::move(prepared).value(), std::move(regions));
}

} // namespace oscillate::application
