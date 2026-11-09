#include "oscillate/application/session/song_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/arrange_from_loop.hpp"
#include "oscillate/application/clip_warp_edit.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/automation/curve.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/effects/effect.hpp"
#include "oscillate/native_dsp/parameters.hpp"
#include "oscillate/native_dsp/synth/engines.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/project/lookup.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;
using foundation::ErrorCode;
using project::find_track;
template <typename T> using Result = foundation::Result<T>;
using project::find_clip;
using Follow = std::pair<const project::Clip*, ClipWarpProposal>;

// The devices Arrange adds, by engine and the names of what it sets.
constexpr std::string_view sweep_engine = "contour";
constexpr std::string_view sweep_model = "contour.model";
constexpr std::string_view sweep_cutoff = "contour.cutoff";
constexpr std::string_view sweep_mix = "contour.mix";
constexpr std::string_view low_pass_model = "LP 24";
constexpr std::string_view high_pass_model = "HP 24";
constexpr std::string_view fill_engine = "chop";
constexpr std::string_view fill_mix = "chop.mix";
constexpr std::string_view transition_engine = "nova";
constexpr double device_off = 0.0;
constexpr double device_on = 1.0;

// Ids for everything one revision creates: numbered tracks, clips and
// markers continue from the session's next numbers; notes, lanes and points
// share a namespace no other revision uses.
struct IdSource {
  std::uint64_t track;
  std::uint64_t clip;
  std::uint64_t marker;
  std::uint64_t space;
  std::uint64_t serial;
  std::uint64_t note = 1;
  std::uint64_t lane = 1;
  std::uint64_t point = 1;
  [[nodiscard]] project::NoteId next_note() {
    return project::NoteId::from_bits({space, note++}).value();
  }
  [[nodiscard]] project::AutomationLaneId next_lane() {
    return project::AutomationLaneId::from_bits({space, lane++}).value();
  }
  [[nodiscard]] project::AutomationPointId next_point() {
    return project::AutomationPointId::from_bits({space, point++}).value();
  }
};

// The runs of bars a part plays, as [first, last).
[[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>>
runs(const arrange::TrackPart& part) {
  std::vector<std::pair<std::int64_t, std::int64_t>> result;
  const auto count = static_cast<std::int64_t>(part.plays.size());
  for (std::int64_t bar = 0; bar < count;) {
    if (!part.plays[static_cast<std::size_t>(bar)]) {
      ++bar;
      continue;
    }
    auto end = bar;
    while (end < count && part.plays[static_cast<std::size_t>(end)])
      ++end;
    result.emplace_back(bar, end);
    bar = end;
  }
  return result;
}

// The loop's notes as a pattern starting `phase` ticks into the loop: a
// note that wraps past the pattern's end continues from its start.
[[nodiscard]] std::vector<project::MidiNote> rotated(const std::vector<project::MidiNote>& notes,
                                                     std::int64_t phase, std::int64_t length,
                                                     IdSource& ids) {
  std::vector<project::MidiNote> result;
  for (const auto& note : notes) {
    const auto duration = note.end.value - note.start.value;
    const auto start = ((note.start.value - phase) % length + length) % length;
    const auto end = start + duration;
    result.push_back({ids.next_note(), {start}, {std::min(end, length)}, note.key, note.velocity});
    if (end > length)
      result.push_back({ids.next_note(), {0}, {end - length}, note.key, note.velocity});
  }
  std::ranges::sort(result, [](const auto& a, const auto& b) {
    return std::tie(a.start.value, a.key) < std::tie(b.start.value, b.key);
  });
  // A wrapped tail landing on a note of the same key at the same tick goes.
  const auto same = std::ranges::unique(result, [](const auto& a, const auto& b) {
    return a.start.value == b.start.value && a.key == b.key;
  });
  result.erase(same.begin(), same.end());
  return result;
}

// A built-in effect at a preset (or its defaults) with some values set.
[[nodiscard]] Result<project::NativeEffectPatch>
effect_patch(std::string_view engine_id, std::optional<std::string_view> preset,
             const std::vector<std::pair<std::string_view, double>>& values) {
  using R = Result<project::NativeEffectPatch>;
  const auto* engine = native_dsp::effects::find_engine(engine_id);
  if (engine == nullptr)
    return R::failure(
        {ErrorCode::not_found, "Built-in effect not found: " + std::string(engine_id)});
  std::optional<std::size_t> index;
  if (preset) {
    const auto& presets = engine->descriptor->presets;
    const auto found = std::ranges::find(presets, *preset, &native_dsp::Preset::name);
    if (found == presets.end())
      return R::failure({ErrorCode::not_found, "Preset not found: " + std::string(*preset)});
    index = static_cast<std::size_t>(found - presets.begin());
  }
  auto base = native_inserts::native_effect_patch_for_preset(*engine, index);
  if (!base)
    return R::failure(base.error());
  auto resolved = native_inserts::resolve_native_effect(base.value());
  if (!resolved)
    return R::failure(resolved.error());
  auto dense = std::move(resolved.value().values);
  for (const auto& [id, value] : values) {
    const auto at = native_dsp::find_parameter(*engine->descriptor, id);
    if (!at)
      return R::failure({ErrorCode::not_found, "No parameter " + std::string(id)});
    dense[*at] = value;
  }
  return native_inserts::make_native_effect_patch(*engine, dense, base.value().document);
}

// One parameter of a built-in effect engine.
struct EffectParameter {
  std::string_view engine_id;
  std::string_view parameter;
};

// The index of a choice parameter's label.
[[nodiscard]] Result<double> choice_index(EffectParameter effect, std::string_view label) {
  const auto parameter = effect.parameter;
  const auto* engine = native_dsp::effects::find_engine(effect.engine_id);
  if (engine == nullptr)
    return Result<double>::failure({ErrorCode::not_found, "Built-in effect not found"});
  const auto at = native_dsp::find_parameter(*engine->descriptor, parameter);
  if (!at)
    return Result<double>::failure(
        {ErrorCode::not_found, "No parameter " + std::string(parameter)});
  const auto& choices = engine->descriptor->parameters[*at].choices;
  const auto found = std::ranges::find(choices, label);
  if (found == choices.end())
    return Result<double>::failure({ErrorCode::not_found, "No choice " + std::string(label)});
  return Result<double>::success(static_cast<double>(found - choices.begin()));
}

[[nodiscard]] Result<project::AutomationDomain> effect_domain(EffectParameter effect) {
  const auto* engine = native_dsp::effects::find_engine(effect.engine_id);
  if (engine == nullptr)
    return Result<project::AutomationDomain>::failure(
        {ErrorCode::not_found, "Built-in effect not found"});
  return native_inserts::native_effect_automation_domain(*engine, effect.parameter);
}

struct LanePoint {
  std::int64_t tick;
  double value;
  project::AutomationShape shape;
};

// A musical lane from points in order; a later point on a taken tick
// replaces it (a fill that starts where another ends joins it).
[[nodiscard]] project::AutomationLane lane_of(project::AutomationTarget target,
                                              project::AutomationDomain domain,
                                              std::vector<LanePoint> points, IdSource& ids) {
  std::ranges::stable_sort(points, {}, &LanePoint::tick);
  project::AutomationLane lane{ids.next_lane(),
                               std::move(target),
                               std::move(domain),
                               project::AutomationTimebase::musical,
                               {},
                               1};
  for (const auto& point : points) {
    if (!lane.points.empty() && lane.points.back().coordinate == point.tick) {
      lane.points.back().value = point.value;
      lane.points.back().to_next = point.shape;
      continue;
    }
    lane.points.push_back({ids.next_point(), point.tick, point.value, point.shape});
  }
  return lane;
}

} // namespace

foundation::Result<void> SongService::set_tempo_event(timeline::TickPosition tick, double bpm,
                                                      timeline::TempoShape shape_to_next) {
  using Result = foundation::Result<void>;
  if (tick.value < 0 || snapshot().tempo.revision() == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "Tempo event position or map revision is out of range"});
  }
  auto events = snapshot().tempo.events();
  const auto existing = std::ranges::lower_bound(events, tick, {}, &timeline::TempoEvent::tick);
  if (existing != events.end() && existing->tick == tick) {
    if (existing->bpm == bpm && existing->shape_to_next == shape_to_next) {
      return Result::success();
    }
    existing->bpm = bpm;
    existing->shape_to_next = shape_to_next;
  } else {
    events.insert(existing, {tick, bpm, shape_to_next});
  }
  auto map = timeline::TempoMap::create(std::move(events), snapshot().tempo.revision() + 1);
  if (!map) {
    return Result::failure(map.error());
  }
  auto applied = model().apply({project::ReplaceTempoMap{std::move(map.value())}});
  if (!applied) {
    return Result::failure(applied.error());
  }
  return Result::success();
}

foundation::Result<void> SongService::remove_tempo_event(timeline::TickPosition tick) {
  using Result = foundation::Result<void>;
  if (tick.value <= 0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "The initial tempo event cannot be removed"});
  }
  if (snapshot().tempo.revision() == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Tempo map revision exhausted"});
  }
  auto events = snapshot().tempo.events();
  const auto existing = std::ranges::lower_bound(events, tick, {}, &timeline::TempoEvent::tick);
  if (existing == events.end() || existing->tick != tick) {
    return Result::failure({foundation::ErrorCode::not_found, "Tempo event not found"});
  }
  events.erase(existing);
  return replace_tempo(std::move(events));
}

foundation::Result<void> SongService::replace_tempo(std::vector<timeline::TempoEvent> events) {
  using Result = foundation::Result<void>;
  if (snapshot().tempo.revision() == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Tempo map revision exhausted"});
  // The last event holds: there is no later tempo for it to move towards.
  if (!events.empty())
    events.back().shape_to_next = timeline::TempoShape::step;
  auto map = timeline::TempoMap::create(std::move(events), snapshot().tempo.revision() + 1);
  if (!map)
    return Result::failure(map.error());
  auto applied = model().apply({project::ReplaceTempoMap{std::move(map.value())}});
  if (!applied)
    return Result::failure(applied.error());
  return Result::success();
}

foundation::Result<void> SongService::move_tempo_event(timeline::TickPosition from,
                                                       timeline::TickPosition to, double bpm) {
  using Result = foundation::Result<void>;
  if (to.value < 0 || (from.value == 0) != (to.value == 0))
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "The first tempo point stays at the start of the song"});
  auto events = snapshot().tempo.events();
  const auto moved = std::ranges::find(events, from, &timeline::TempoEvent::tick);
  if (moved == events.end())
    return Result::failure({foundation::ErrorCode::not_found, "Tempo event not found"});
  const auto shape = moved->shape_to_next;
  if (moved->tick == to && moved->bpm == bpm)
    return Result::success();
  events.erase(moved);
  const auto at = std::ranges::lower_bound(events, to, {}, &timeline::TempoEvent::tick);
  if (at != events.end() && at->tick == to)
    return Result::failure(
        {foundation::ErrorCode::conflict, "Another tempo point is already there"});
  events.insert(at, {to, bpm, shape});
  return replace_tempo(std::move(events));
}

foundation::Result<void> SongService::set_tempo_ramp(timeline::TickPosition from,
                                                     timeline::TickPosition to, double end_bpm,
                                                     timeline::TempoShape shape) {
  using Result = foundation::Result<void>;
  if (from.value < 0 || to <= from)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "A tempo ramp needs a range of the song"});
  const auto& tempo = snapshot().tempo;
  auto events = tempo.events();
  // The tempo playing at the ramp's start, before the edit.
  double start_bpm = 0.0;
  if (const auto existing = std::ranges::find(events, from, &timeline::TempoEvent::tick);
      existing != events.end()) {
    start_bpm = existing->bpm;
  } else {
    auto sample = tempo.tick_to_sample(from, snapshot().sample_rate);
    if (!sample || !tempo.bpm_at_sample_rt(sample.value(), snapshot().sample_rate, start_bpm))
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "The ramp's start is outside the song"});
  }
  std::erase_if(events, [&](const timeline::TempoEvent& event) {
    return event.tick >= from && event.tick <= to;
  });
  const auto at = std::ranges::lower_bound(events, from, {}, &timeline::TempoEvent::tick);
  events.insert(at, {{from, start_bpm, shape}, {to, end_bpm, timeline::TempoShape::step}});
  return replace_tempo(std::move(events));
}

foundation::Result<void> SongService::ramp_to_clip_tempo(project::ClipId clip) {
  auto ramp = plan_transition_ramp(snapshot(), clip);
  if (!ramp)
    return foundation::Result<void>::failure(ramp.error());
  return set_tempo_ramp(ramp.value().from, ramp.value().to, ramp.value().bpm,
                        transition_ramp_shape);
}

foundation::Result<void> SongService::set_initial_meter(std::uint8_t numerator,
                                                        std::uint8_t denominator) {
  using Result = foundation::Result<void>;
  const auto& saved = snapshot().meter;
  if (saved.events().empty() || saved.revision() == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Meter map cannot be revised"});
  }
  if (saved.events().front().numerator == numerator &&
      saved.events().front().denominator == denominator) {
    return Result::success();
  }
  auto events = saved.events();
  events.front().numerator = numerator;
  events.front().denominator = denominator;
  auto map = timeline::MeterMap::create(std::move(events), saved.revision() + 1);
  if (!map)
    return Result::failure(map.error());
  auto applied = model().apply({project::ReplaceMeterMap{std::move(map.value())}});
  return applied ? Result::success() : Result::failure(applied.error());
}

std::uint64_t SongService::next_marker_number() const {
  std::uint64_t greatest = snapshot().revision;
  for (const auto& marker : snapshot().markers) {
    if (marker.id.bits().high == 0)
      greatest = std::max(greatest, marker.id.bits().low);
  }
  return next_number(greatest);
}

foundation::Result<void> SongService::set_project_key(std::optional<project::MusicalKey> key) {
  auto applied = model().apply({project::SetProjectKey{key}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

std::optional<project::MusicalKey> SongService::suggested_project_key() const {
  const auto& state = snapshot();
  std::vector<std::pair<project::MusicalKey, std::size_t>> counts;
  for (const auto& review : state.source_key_reviews) {
    const auto key = review.correction ? review.correction : review.detected;
    const auto used = std::ranges::any_of(state.tracks, [&](const project::Track& track) {
      return std::ranges::any_of(
          track.clips, [&](const project::Clip& clip) { return clip.source == review.source; });
    });
    if (!key || !used)
      continue;
    const auto found =
        std::ranges::find(counts, *key, &std::pair<project::MusicalKey, std::size_t>::first);
    if (found == counts.end())
      counts.emplace_back(*key, 1);
    else
      ++found->second;
  }
  // The most common key; the first reviewed wins a tie.
  const auto best =
      std::ranges::max_element(counts, std::less{}, [](const auto& entry) { return entry.second; });
  return best == counts.end() ? std::nullopt : std::optional<project::MusicalKey>{best->first};
}

foundation::Result<project::MarkerId> SongService::add_marker(timeline::TickPosition position,
                                                              std::string name) {
  using Result = foundation::Result<project::MarkerId>;
  const auto existing = std::ranges::find(snapshot().markers, position, &project::Marker::position);
  if (existing != snapshot().markers.end()) {
    const auto id = existing->id;
    if (!name.empty() && name != existing->name) {
      auto renamed = model().apply({project::RenameMarker{id, std::move(name)}});
      if (!renamed)
        return Result::failure(renamed.error());
    }
    return Result::success(id);
  }
  auto id = numbered_id<project::MarkerId>(next_marker_number());
  if (!id)
    return Result::failure(id.error());
  auto applied = model().apply({project::AddMarker{{id.value(), position, std::move(name)}}});
  return applied ? Result::success(id.value()) : Result::failure(applied.error());
}

foundation::Result<void> SongService::rename_marker(project::MarkerId marker, std::string name) {
  auto applied = model().apply({project::RenameMarker{marker, std::move(name)}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> SongService::move_marker(project::MarkerId marker,
                                                  timeline::TickPosition position) {
  auto applied = model().apply({project::MoveMarker{marker, position}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

foundation::Result<void> SongService::remove_marker(project::MarkerId marker) {
  auto applied = model().apply({project::RemoveMarker{marker}});
  return applied ? foundation::Result<void>::success()
                 : foundation::Result<void>::failure(applied.error());
}

std::optional<project::Marker> SongService::next_marker(timeline::TickPosition position) const {
  const auto found = std::ranges::find_if(snapshot().markers, [&](const project::Marker& marker) {
    return marker.position > position;
  });
  if (found == snapshot().markers.end())
    return std::nullopt;
  return *found;
}

std::optional<project::Marker> SongService::previous_marker(timeline::TickPosition position) const {
  std::optional<project::Marker> previous;
  for (const auto& marker : snapshot().markers) {
    if (marker.position >= position)
      break;
    previous = marker;
  }
  return previous;
}

Result<void> SongService::apply_arrangement(const arrange::Plan& plan) {
  using R = Result<void>;
  const auto& state = snapshot();
  if (plan.revision != state.revision)
    return R::failure(
        {ErrorCode::conflict, "The project changed since this arrangement was planned"});
  if (plan.parts.empty())
    return R::failure({ErrorCode::invalid_argument, "Nothing in the loop to arrange"});
  for (const auto& finding : plan.findings)
    if (finding.locked && std::ranges::find(plan.parts, finding.track,
                                            &arrange::TrackPart::track) != plan.parts.end())
      return R::failure({ErrorCode::invalid_argument, "A locked track has a part"});

  // The arrangement's bar lines, from bar 1.
  std::vector<std::int64_t> lines;
  lines.reserve(static_cast<std::size_t>(plan.length_bars) + 1);
  for (std::int64_t bar = 0; bar <= plan.length_bars; ++bar) {
    auto tick = state.meter.tick_at({bar + 1, 1, 0});
    if (!tick)
      return R::failure(tick.error());
    lines.push_back(tick.value().value);
  }
  const auto at = [&](double bar) {
    const auto whole =
        std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(bar)), 0, plan.length_bars);
    if (whole == plan.length_bars)
      return lines.back();
    const auto fraction = bar - static_cast<double>(whole);
    const auto width =
        lines[static_cast<std::size_t>(whole) + 1] - lines[static_cast<std::size_t>(whole)];
    return lines[static_cast<std::size_t>(whole)] +
           static_cast<std::int64_t>(std::llround(fraction * static_cast<double>(width)));
  };
  const auto origin = lines.front();
  const auto loop_ticks = plan.loop.ticks();
  if (loop_ticks <= 0)
    return R::failure({ErrorCode::invalid_argument, "The loop is empty"});

  const auto space =
      state.revision == std::numeric_limits<std::uint64_t>::max() ? 0 : state.revision + 1;
  if (space == 0)
    return R::failure({ErrorCode::resource_exhausted, "Project ID space exhausted"});
  IdSource ids{next_track_number(), next_clip_number(), next_marker_number(), space,
               state.next_insert_slot_serial};
  std::vector<project::Operation> operations;

  // Parts: the loop copied where each one plays.
  for (const auto& part : plan.parts) {
    const auto* track = find_track(state, part.track);
    if (track == nullptr)
      return R::failure({ErrorCode::not_found, "A planned track is gone"});
    auto content = arrange::loop_content(state, *track, plan.loop);
    if (!content)
      return R::failure(content.error());
    for (const auto& clip : track->clips)
      operations.emplace_back(project::RemoveClip{clip.id});
    for (const auto& clip : track->midi_clips)
      operations.emplace_back(project::RemoveClip{clip.id});
    for (const auto& [first, last] : runs(part)) {
      const auto start = lines[static_cast<std::size_t>(first)];
      const auto end = lines[static_cast<std::size_t>(last)];
      if (!content.value().notes.empty()) {
        auto id = numbered_id<project::ClipId>(ids.clip++);
        if (!id)
          return R::failure(id.error());
        const auto phase = (start - origin) % loop_ticks;
        operations.emplace_back(project::AddMidiClip{
            part.track, project::MidiClip{id.value(),
                                          project::MusicalInterval{{start}, {end}},
                                          {loop_ticks},
                                          true,
                                          rotated(content.value().notes, phase, loop_ticks, ids)}});
      }
      for (auto pass = (start - origin) / loop_ticks;; ++pass) {
        const auto pass_start = origin + pass * loop_ticks;
        if (pass_start >= end)
          break;
        for (const auto& audio : content.value().audio) {
          const auto clip_start = pass_start + audio.offset_ticks;
          auto copy = audio.clip;
          if (std::holds_alternative<project::MusicalInterval>(copy.placement)) {
            const auto clip_end = clip_start + audio.length;
            if (clip_start < start || clip_end > end)
              continue;
            copy.placement = project::MusicalInterval{{clip_start}, {clip_end}};
          } else {
            auto sample = state.tempo.tick_to_sample({clip_start}, state.sample_rate);
            if (!sample)
              return R::failure(sample.error());
            const timeline::SamplePosition stop{sample.value().value + audio.length};
            auto stop_tick = state.tempo.sample_to_tick(stop, state.sample_rate);
            if (!stop_tick)
              return R::failure(stop_tick.error());
            if (clip_start < start || stop_tick.value().value > end)
              continue;
            copy.placement = project::ElapsedInterval{sample.value(), stop};
          }
          auto id = numbered_id<project::ClipId>(ids.clip++);
          if (!id)
            return R::failure(id.error());
          copy.id = id.value();
          for (auto& lane : copy.automation) {
            lane.id = ids.next_lane();
            for (auto& point : lane.points)
              point.id = ids.next_point();
          }
          operations.emplace_back(project::AddClip{part.track, std::move(copy)});
        }
      }
    }
  }

  // Devices and lanes, per track: a Contour for each kind of sweep, a Chop
  // for fills, and one lane per thrown send.
  std::map<project::TrackId, std::size_t> added;
  const auto add_effect = [&](project::TrackId track_id,
                              project::NativeEffectPatch patch) -> Result<project::InsertSlotId> {
    const auto* track = find_track(state, track_id);
    auto slot = project::command_insert_slot_id(ids.serial++);
    if (!slot)
      return slot;
    const auto index = track->effects.size() + added[track_id]++;
    operations.emplace_back(project::AddNativeEffect{
        track_id, index, project::NativeEffectProcessor{std::move(patch)}});
    return slot;
  };

  for (const bool high_pass : {false, true}) {
    std::map<project::TrackId, std::vector<const arrange::Sweep*>> by_track;
    for (const auto& sweep : plan.sweeps)
      if (sweep.high_pass == high_pass)
        by_track[sweep.track].push_back(&sweep);
    for (const auto& [track_id, sweeps] : by_track) {
      auto model = choice_index({.engine_id = sweep_engine, .parameter = sweep_model},
                                high_pass ? high_pass_model : low_pass_model);
      if (!model)
        return R::failure(model.error());
      auto patch = effect_patch(sweep_engine, std::nullopt,
                                {{sweep_model, model.value()},
                                 {sweep_cutoff, sweeps.front()->from_hz},
                                 {sweep_mix, device_off}});
      if (!patch)
        return R::failure(patch.error());
      auto slot = add_effect(track_id, std::move(patch).value());
      if (!slot)
        return R::failure(slot.error());
      auto mix_domain = effect_domain({.engine_id = sweep_engine, .parameter = sweep_mix});
      auto cutoff_domain = effect_domain({.engine_id = sweep_engine, .parameter = sweep_cutoff});
      if (!mix_domain || !cutoff_domain)
        return R::failure(!mix_domain ? mix_domain.error() : cutoff_domain.error());
      std::vector<LanePoint> mix{{origin, device_off, project::AutomationShape::hold}};
      std::vector<LanePoint> cutoff{
          {origin, sweeps.front()->from_hz, project::AutomationShape::hold}};
      for (const auto* sweep : sweeps) {
        mix.push_back(
            {at(static_cast<double>(sweep->start_bar)), device_on, project::AutomationShape::hold});
        mix.push_back(
            {at(static_cast<double>(sweep->end_bar)), device_off, project::AutomationShape::hold});
        cutoff.push_back({at(static_cast<double>(sweep->start_bar)), sweep->from_hz,
                          project::AutomationShape::linear});
        cutoff.push_back({at(static_cast<double>(sweep->end_bar)), sweep->to_hz,
                          project::AutomationShape::hold});
      }
      operations.emplace_back(project::AddAutomationLane{
          track_id,
          lane_of(project::NativeEffectParameterTarget{slot.value(), std::string(sweep_mix)},
                  mix_domain.value(), std::move(mix), ids)});
      operations.emplace_back(project::AddAutomationLane{
          track_id,
          lane_of(project::NativeEffectParameterTarget{slot.value(), std::string(sweep_cutoff)},
                  cutoff_domain.value(), std::move(cutoff), ids)});
    }
  }

  std::map<project::TrackId, std::vector<std::int64_t>> fills;
  for (const auto& fill : plan.fills)
    fills[fill.track].push_back(fill.bar);
  for (const auto& [track_id, bars] : fills) {
    auto patch = effect_patch(fill_engine, arrange::fill_preset, {{fill_mix, device_off}});
    if (!patch)
      return R::failure(patch.error());
    auto slot = add_effect(track_id, std::move(patch).value());
    if (!slot)
      return R::failure(slot.error());
    auto domain = effect_domain({.engine_id = fill_engine, .parameter = fill_mix});
    if (!domain)
      return R::failure(domain.error());
    std::vector<LanePoint> mix{{origin, device_off, project::AutomationShape::hold}};
    for (const auto bar : bars) {
      mix.push_back({at(static_cast<double>(bar)), device_on, project::AutomationShape::hold});
      mix.push_back({at(static_cast<double>(bar + 1)), device_off, project::AutomationShape::hold});
    }
    operations.emplace_back(project::AddAutomationLane{
        track_id, lane_of(project::NativeEffectParameterTarget{slot.value(), std::string(fill_mix)},
                          domain.value(), std::move(mix), ids)});
  }

  std::map<std::pair<project::TrackId, project::SendId>, std::vector<const arrange::Throw*>> throws;
  for (const auto& thrown : plan.throws)
    throws[{thrown.track, thrown.send}].push_back(&thrown);
  for (const auto& [key, list] : throws) {
    const auto [track_id, send_id] = key;
    const auto* track = find_track(state, track_id);
    const auto send = std::ranges::find(track->sends, send_id, &project::SendRoute::id);
    if (send == track->sends.end())
      return R::failure({ErrorCode::not_found, "A planned send is gone"});
    auto domain = automation_.automation_domain(track_id, project::SendGainTarget{send_id});
    if (!domain)
      return R::failure(domain.error());
    std::vector<LanePoint> gain{{origin, send->gain, project::AutomationShape::hold}};
    for (const auto* thrown : list) {
      gain.push_back({at(thrown->start_bar), thrown->gain, project::AutomationShape::hold});
      gain.push_back(
          {at(thrown->start_bar + thrown->bars), send->gain, project::AutomationShape::hold});
    }
    operations.emplace_back(project::AddAutomationLane{
        track_id, lane_of(project::SendGainTarget{send_id}, domain.value(), std::move(gain), ids)});
  }

  // Transitions: one Nova track per kind, a note per use.
  if (!plan.transitions.empty()) {
    const auto* nova = native_dsp::synth::find_engine(transition_engine);
    if (nova == nullptr)
      return R::failure({ErrorCode::not_found, "Nova is not available for the transitions"});
    const auto& presets = nova->descriptor->presets;
    for (std::size_t kind = 0; kind < arrange::transition_kind_count; ++kind) {
      std::vector<const arrange::Transition*> uses;
      for (const auto& transition : plan.transitions)
        if (static_cast<std::size_t>(transition.kind) == kind)
          uses.push_back(&transition);
      if (uses.empty())
        continue;
      const auto& facts = arrange::transition_facts[kind];
      const auto preset = std::ranges::find(presets, facts.nova_preset, &native_dsp::Preset::name);
      if (preset == presets.end())
        return R::failure(
            {ErrorCode::not_found, "Nova preset not found: " + std::string(facts.nova_preset)});
      auto patch = native_inserts::native_patch_for_preset(
          *nova, static_cast<std::size_t>(preset - presets.begin()));
      if (!patch)
        return R::failure(patch.error());
      auto track_id = numbered_id<project::TrackId>(ids.track++);
      if (!track_id)
        return R::failure(track_id.error());
      operations.emplace_back(
          project::AddTrack{track_id.value(), project::TrackKind::instrument, std::nullopt});
      operations.emplace_back(
          project::SetNativeInstrumentPatch{track_id.value(), std::move(patch).value()});
      operations.emplace_back(
          project::SetTrackName{track_id.value(), std::string(facts.track_name)});
      for (const auto* use : uses) {
        const auto start = at(static_cast<double>(use->start_bar));
        const auto end = at(static_cast<double>(use->start_bar + use->bars));
        auto clip_id = numbered_id<project::ClipId>(ids.clip++);
        if (!clip_id)
          return R::failure(clip_id.error());
        operations.emplace_back(project::AddMidiClip{
            track_id.value(),
            project::MidiClip{clip_id.value(),
                              project::MusicalInterval{{start}, {end}},
                              {end - start},
                              false,
                              {{ids.next_note(), {0}, {end - start}, facts.key, facts.velocity}}}});
      }
    }
  }

  // A marker at each section, where none is yet.
  for (const auto& section : plan.sections) {
    const auto tick = at(static_cast<double>(section.start_bar));
    if (std::ranges::find(state.markers, tick, [](const project::Marker& marker) {
          return marker.position.value;
        }) != state.markers.end())
      continue;
    auto id = numbered_id<project::MarkerId>(ids.marker++);
    if (!id)
      return R::failure(id.error());
    operations.emplace_back(project::AddMarker{
        project::Marker{id.value(), {tick}, std::string(arrange::section_name(section.kind))}});
  }

  auto applied = model().apply(operations);
  return applied ? R::success() : R::failure(applied.error());
}

Result<void> SongService::insert_lead_bars(std::uint32_t bars) {
  std::vector<project::Operation> operations;
  auto shift = lead_bar_operations(snapshot(), bars, {}, operations);
  if (!shift)
    return Result<void>::failure(shift.error());
  auto applied = model().apply(operations);
  if (!applied)
    return Result<void>::failure(applied.error());
  return Result<void>::success();
}

} // namespace oscillate::application
