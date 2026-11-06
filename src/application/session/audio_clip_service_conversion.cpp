#include "oscillate/application/session/audio_clip_service.hpp"

#include "edit_support.hpp"
#include "oscillate/application/crate_chops.hpp"
#include "oscillate/native_dsp/synth/crate.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/processing/clip_frame_plan.hpp"
#include "oscillate/source_prep/synth_media.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using foundation::ErrorCode;
template <typename T> using Result = foundation::Result<T>;
namespace chops = crate;
namespace facts = native_dsp::synth::crate;

// The engine's "Bend Range" parameter, by name: engines name its id
// differently ("nova.global.bend_range", "ember.voice.bend", ...).
[[nodiscard]] const native_dsp::synth::ParameterDescriptor*
bend_range_parameter(const native_dsp::synth::SynthEngine& engine) {
  for (const auto& parameter : engine.descriptor->parameters)
    if (parameter.name == clay::bend_range_name)
      return &parameter;
  return nullptr;
}

struct ClipLocation {
  const project::Track* track = nullptr;
  std::size_t row = 0;
  const project::Clip* clip = nullptr;
  bool midi = false;
};

[[nodiscard]] ClipLocation find_clip(const project::Snapshot& state, project::ClipId id) {
  for (std::size_t row = 0; row < state.tracks.size(); ++row) {
    const auto& track = state.tracks[row];
    if (const auto audio = std::ranges::find(track.clips, id, &project::Clip::id);
        audio != track.clips.end())
      return {&track, row, &*audio, false};
    if (std::ranges::find(track.midi_clips, id, &project::MidiClip::id) != track.midi_clips.end())
      return {&track, row, nullptr, true};
  }
  return {};
}

// The source's tempo as the project knows it (the user's correction, else
// the reviewed detection), for Crate's Follow Tempo.
[[nodiscard]] std::optional<double> source_bpm(const project::Snapshot& state,
                                               project::SourceId source) {
  const auto review =
      std::ranges::find(state.source_tempo_reviews, source, &project::SourceTempoReview::source);
  if (review == state.source_tempo_reviews.end())
    return std::nullopt;
  const double bpm = review->corrected_bpm.value_or(review->detected_bpm);
  return bpm > 0.0 ? std::optional<double>(bpm) : std::nullopt;
}

} // namespace

Result<void> AudioClipService::set_clip_clay(project::ClipId clip,
                                             std::optional<project::ClayTake> take) {
  auto found = clay::find_clip(snapshot(), clip);
  if (!found)
    return Result<void>::failure(found.error());
  if (found.value().clip->clay == take)
    return Result<void>::success();
  auto applied = model().apply({project::SetClipClay{clip, std::move(take)}});
  return applied ? Result<void>::success() : Result<void>::failure(applied.error());
}

Result<project::ClipId> AudioClipService::add_clay_midi_clip(std::optional<project::TrackId> track,
                                                             const clay::MidiClipOut& midi) {
  using R = Result<project::ClipId>;
  if (midi.notes.empty())
    return R::failure({ErrorCode::invalid_argument, "There are no notes to send to MIDI"});
  if (midi.notes.size() > project::max_midi_notes_per_clip)
    return R::failure({ErrorCode::resource_exhausted, "Too many notes for a clip"});
  std::vector<project::Operation> operations;
  project::NativeInstrumentPatch patch;
  std::optional<project::TrackId> chosen;
  if (track) {
    const auto found = std::ranges::find(snapshot().tracks, *track, &project::Track::id);
    if (found == snapshot().tracks.end())
      return R::failure({ErrorCode::not_found, "That track is not in the project"});
    const auto* saved = std::get_if<project::NativeInstrumentPatch>(&found->instrument_source);
    if (found->kind != project::TrackKind::instrument || saved == nullptr)
      return R::failure({ErrorCode::unavailable, "To MIDI needs a built-in synth track"});
    patch = *saved;
    chosen = *track;
  } else {
    const auto* engine = native_dsp::synth::find_engine(clay::to_midi_engine);
    if (engine == nullptr)
      return R::failure({ErrorCode::not_found, "Built-in synth not found"});
    auto id = numbered_id<project::TrackId>(next_track_number());
    if (!id)
      return R::failure(id.error());
    auto made = native_dsp::synth::native_patch_for_preset(*engine, std::nullopt);
    if (!made)
      return R::failure(made.error());
    chosen = id.value();
    patch = std::move(made).value();
    operations.emplace_back(
        project::AddTrack{*chosen, project::TrackKind::instrument, std::nullopt});
    operations.emplace_back(project::SetNativeInstrumentPatch{*chosen, patch});
  }
  const auto target = *chosen;
  auto resolved = native_dsp::synth::resolve_native_instrument(patch);
  if (!resolved)
    return R::failure(resolved.error());
  const auto& engine = *resolved.value().engine;
  auto clip_id = numbered_id<project::ClipId>(next_clip_number());
  if (!clip_id)
    return R::failure(clip_id.error());
  auto notes = midi.notes;
  const auto first_note = next_note_number();
  if (first_note == 0 || first_note > std::numeric_limits<std::uint64_t>::max() - notes.size())
    return R::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  for (std::size_t index = 0; index < notes.size(); ++index) {
    auto note_id = numbered_id<project::NoteId>(first_note + index);
    if (!note_id)
      return R::failure(note_id.error());
    notes[index].id = note_id.value();
  }
  std::vector<project::AutomationLane> lanes;
  const auto* range = bend_range_parameter(engine);
  if (!midi.bends.empty() && range != nullptr) {
    const auto parameter =
        std::string(engine.descriptor->id) + std::string(clay::bend_parameter_suffix);
    auto domain = native_dsp::synth::native_automation_domain(engine, parameter);
    if (!domain)
      return R::failure(domain.error());
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    if (!lane_id)
      return R::failure(lane_id.error());
    project::AutomationLane lane{lane_id.value(),
                                 project::NativeParameterTarget{target, parameter},
                                 std::move(domain).value(),
                                 project::AutomationTimebase::musical,
                                 {},
                                 1};
    // Raise the bend range when the bends need more of it; the points are
    // fractions of the range the engine then has.
    const auto index = static_cast<std::size_t>(range - engine.descriptor->parameters.data());
    const double current = resolved.value().values[index];
    const double wanted = std::min(range->maximum, midi.bend_range);
    const double effective = std::max(current, wanted);
    if (!(effective > 0.0))
      return R::failure({ErrorCode::unavailable, "The synth's bend range is zero"});
    for (std::size_t point = 0; point < midi.bends.size(); ++point) {
      auto point_id = numbered_id<project::AutomationPointId>(point_number + point);
      if (!point_id)
        return R::failure(point_id.error());
      const double value =
          std::clamp(midi.bends[point].value * midi.bend_range / effective, -1.0, 1.0);
      lane.points.push_back(
          {point_id.value(), midi.bends[point].tick, value, project::AutomationShape::linear});
    }
    lanes.push_back(std::move(lane));
    if (wanted > current) {
      auto raised = patch;
      const auto id = std::string(range->id);
      const auto saved =
          std::ranges::find(raised.parameters, id, &project::NativeInstrumentParameter::id);
      if (saved == raised.parameters.end())
        raised.parameters.push_back({id, wanted});
      else
        saved->value = wanted;
      operations.emplace_back(project::SetNativeInstrumentPatch{target, std::move(raised)});
    }
  }
  project::MidiClip clip{
      clip_id.value(),
      project::MusicalInterval{midi.start, {midi.start.value + midi.length.value}},
      midi.length,
      false,
      std::move(notes),
      1,
      std::move(lanes)};
  operations.emplace_back(project::AddMidiClip{target, std::move(clip)});
  auto applied = model().apply(std::move(operations));
  if (!applied)
    return R::failure(applied.error());
  return R::success(clip_id.value());
}

Result<SamplerConversion> AudioClipService::convert_audio_clip_to_sampler(project::ClipId clip_id) {
  using R = Result<SamplerConversion>;
  if (state_.repair_state)
    return R::failure(repair_in_progress());
  const auto& state = snapshot();
  const auto found = find_clip(state, clip_id);
  if (found.midi)
    return R::failure({ErrorCode::invalid_argument, "Convert to sampler takes an audio clip"});
  if (found.clip == nullptr)
    return R::failure({ErrorCode::not_found, "Audio clip to convert not found"});
  const auto& owner = *found.track;
  const auto& clip = *found.clip;
  if (owner.frozen)
    return R::failure({ErrorCode::conflict, "Unfreeze the track to convert its clips"});
  const auto source = std::ranges::find(state.sources, clip.source, &project::SourceRecord::id);
  if (source == state.sources.end())
    return R::failure({ErrorCode::not_found, "The clip's audio is missing from the project"});
  if (source->external_link)
    return R::failure({ErrorCode::unsupported,
                       "The sampler plays audio stored in the project; this clip's file is "
                       "linked from outside it"});
  if (source->channels == 0 || source->channels > 2)
    return R::failure({ErrorCode::unsupported, "The sampler plays mono or stereo audio"});
  if (source->frames > project::max_native_synth_media_frames)
    return R::failure(
        {ErrorCode::resource_exhausted,
         "Clip too long for the sampler (max " +
             std::to_string(project::max_native_synth_media_frames / source->sample_rate) + " s)"});
  const auto* engine = native_dsp::synth::find_engine(facts::engine_id);
  if (engine == nullptr || engine->descriptor == nullptr)
    return R::failure({ErrorCode::unavailable, "The Crate sampler is not available"});

  // Where each source frame sounds: the clip's own frame plan, exact for
  // unwarped, stretched and warp-marked clips alike.
  auto plan = processing::prepare_clip_frame_plan(clip, *source, state.tempo, state.sample_rate);
  if (!plan)
    return R::failure(plan.error());

  // Crate's media slot names the clip's source; decoding it through that
  // slot checks the managed asset as playback will.
  const project::ManagedAudioReference reference{source->id,          source->sha256,
                                                 source->byte_length, source->sample_rate,
                                                 source->channels,    source->frames};
  project::NativeSynthDocument document;
  document.media.push_back({std::string(chops::crate_sample_media), reference, 0});
  auto media = source_prep::load_synth_media(state_.store, state, document);
  if (!media)
    return R::failure(
        {media.error().code, "The clip's audio is not available: " + media.error().message});
  const auto decoded = std::ranges::find(media.value(), chops::crate_sample_media,
                                         &native_dsp::synth::SynthMediaEntry::slot);
  if (decoded == media.value().end() || !decoded->asset)
    return R::failure({ErrorCode::unavailable, "The clip's audio could not be decoded"});
  const auto& audio = *decoded->asset;

  // Transients of what the clip plays, then the chops in source frames.
  const auto first = static_cast<std::uint64_t>(clip.source_range.start.value);
  const auto end = static_cast<std::uint64_t>(clip.source_range.end.value);
  if (first >= end || end > audio.frames.size())
    return R::failure({ErrorCode::corrupt_data, "The clip plays outside its audio"});
  chops::SampleAudio part;
  const chops::SampleAudio* detected = &audio;
  if (first > 0 || end < audio.frames.size()) {
    part.source_rate = audio.source_rate;
    part.frames.assign(std::next(audio.frames.begin(), static_cast<std::ptrdiff_t>(first)),
                       std::next(audio.frames.begin(), static_cast<std::ptrdiff_t>(end)));
    detected = &part;
  }
  auto hits =
      chops::chop_transients(*detected, static_cast<float>(chops::default_chop_sensitivity));
  if (!hits)
    return R::failure(hits.error());
  auto starts = chops::range_chop_starts(hits.value(), first, end, audio);
  if (!starts)
    return R::failure(starts.error());

  // The patch a sample loaded on the panel gets: its chops, root and labels.
  auto name = clip.name.empty() ? owner.name : clip.name;
  auto sample = chops::new_sample_state(*engine->descriptor,
                                        native_dsp::synth::default_values(*engine->descriptor), {},
                                        starts.value(), audio, source_bpm(state, source->id), name);
  if (!sample)
    return R::failure(sample.error());
  const auto ordered = chops::region_starts(sample.value().regions);
  document.regions = std::move(sample.value().regions);
  document.labels = std::move(sample.value().labels);
  auto patch =
      native_dsp::synth::make_native_patch(*engine, sample.value().values, std::move(document));
  if (!patch)
    return R::failure(patch.error());

  // The MIDI clip sits where the audio clip does, in its timebase. Note
  // ticks are relative to the clip's anchor tick, as playback reads them.
  std::int64_t anchor = 0;
  std::int64_t last = 0;
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    anchor = musical->start.value;
    last = musical->end.value;
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    auto from = state.tempo.sample_to_tick(elapsed.start, state.sample_rate);
    auto to = state.tempo.sample_to_tick(elapsed.end, state.sample_rate);
    if (!from || !to)
      return R::failure(!from ? from.error() : to.error());
    anchor = from.value().value;
    last = to.value().value;
  }
  const auto length = last - anchor;
  if (length <= 0)
    return R::failure({ErrorCode::invalid_argument, "The clip has no length to convert"});
  const auto tick_of = [&](std::uint64_t frame) -> Result<std::int64_t> {
    const auto output = plan.value().warp.output_at_source(frame - first);
    if (!output)
      return Result<std::int64_t>::failure(
          {ErrorCode::invalid_argument, "A chop lies outside the clip"});
    const auto sample_frame = plan.value().project_start_frame + *output;
    if (sample_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return Result<std::int64_t>::failure(
          {ErrorCode::resource_exhausted, "The clip is too far into the song"});
    auto tick =
        state.tempo.sample_to_tick({static_cast<std::int64_t>(sample_frame)}, state.sample_rate);
    if (!tick)
      return Result<std::int64_t>::failure(tick.error());
    return Result<std::int64_t>::success(
        std::clamp<std::int64_t>(tick.value().value - anchor, 0, length));
  };

  // One note per chop the clip plays, on its pad (key first_pad_key + its
  // index in time order), from where the chop sounds until the next one or
  // the clip's end. The lead-in before the clip and the tail after it play
  // nothing.
  const auto first_note = next_note_number();
  if (first_note == 0 || first_note > std::numeric_limits<std::uint64_t>::max() - ordered.size())
    return R::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  std::vector<project::MidiNote> notes;
  notes.reserve(ordered.size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const auto from = std::max(ordered[index], first);
    const auto to = std::min(index + 1 < ordered.size() ? ordered[index + 1] : end, end);
    if (to <= from)
      continue;
    auto note_start = tick_of(from);
    auto note_end = tick_of(to);
    if (!note_start || !note_end)
      return R::failure(!note_start ? note_start.error() : note_end.error());
    if (note_end.value() <= note_start.value())
      continue;
    auto note_id = numbered_id<project::NoteId>(first_note + notes.size());
    if (!note_id)
      return R::failure(note_id.error());
    notes.push_back({note_id.value(),
                     {note_start.value()},
                     {note_end.value()},
                     static_cast<std::uint8_t>(facts::first_pad_key + index),
                     chops::chop_note_velocity});
  }
  if (notes.empty())
    return R::failure({ErrorCode::invalid_argument, "The clip is too short to chop"});

  auto track_id = numbered_id<project::TrackId>(next_track_number());
  auto midi_id = numbered_id<project::ClipId>(next_clip_number());
  if (!track_id || !midi_id)
    return R::failure(!track_id ? track_id.error() : midi_id.error());
  const auto chop_count = notes.size();
  project::MidiClip midi{midi_id.value(), clip.placement, {length}, false, std::move(notes)};
  midi.color = clip.color;

  std::vector<project::Operation> operations;
  operations.emplace_back(
      project::AddTrack{track_id.value(), project::TrackKind::instrument, found.row + 1});
  operations.emplace_back(
      project::SetNativeInstrumentPatch{track_id.value(), std::move(patch).value()});
  if (auto track_name = chops::utf8_prefix(std::move(name), project::max_track_name_bytes);
      !track_name.empty())
    operations.emplace_back(project::SetTrackName{track_id.value(), std::move(track_name)});
  if (owner.color)
    operations.emplace_back(project::SetTrackColor{track_id.value(), owner.color});
  operations.emplace_back(project::AddMidiClip{track_id.value(), std::move(midi)});
  auto applied = model().apply(operations);
  if (!applied)
    return R::failure(applied.error());
  return R::success({track_id.value(), midi_id.value(), chop_count});
}

} // namespace oscillate::application
