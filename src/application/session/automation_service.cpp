#include "oscillate/application/session/automation_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_dsp/synth/project_binding.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

struct AudioClipOwner {
  const project::Track* track;
  const project::Clip* clip;
};

[[nodiscard]] std::optional<AudioClipOwner> find_audio_clip(const project::Snapshot& snapshot,
                                                            project::ClipId id) {
  for (const auto& track : snapshot.tracks) {
    const auto clip = std::ranges::find(track.clips, id, &project::Clip::id);
    if (clip != track.clips.end())
      return AudioClipOwner{&track, &*clip};
  }
  return std::nullopt;
}

[[nodiscard]] const project::AutomationLane* find_clip_channel_lane(const AudioClipOwner& owner) {
  const project::AutomationTarget target =
      project::TrackControlTarget{owner.track->id, project::TrackControlTarget::Key::channel_level};
  const auto lane = std::ranges::find_if(
      owner.clip->automation, [&target](const auto& item) { return item.target == target; });
  return lane == owner.clip->automation.end() ? nullptr : &*lane;
}

[[nodiscard]] foundation::Result<std::int64_t>
clip_channel_coordinate(const project::Snapshot& snapshot, const project::Clip& clip,
                        project::AutomationTimebase timebase, timeline::TickPosition tick) {
  using Result = foundation::Result<std::int64_t>;
  auto target = snapshot.tempo.tick_to_sample(tick, snapshot.sample_rate);
  if (!target)
    return Result::failure(target.error());
  timeline::TickPosition start_tick{};
  timeline::SamplePosition start_sample{};
  timeline::SamplePosition end_sample{};
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    if (tick < musical->start || tick >= musical->end)
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "Channel point must be inside its audio clip"});
    start_tick = musical->start;
    auto begin = snapshot.tempo.tick_to_sample(musical->start, snapshot.sample_rate);
    auto end = snapshot.tempo.tick_to_sample(musical->end, snapshot.sample_rate);
    if (!begin || !end)
      return Result::failure(!begin ? begin.error() : end.error());
    start_sample = begin.value();
    end_sample = end.value();
  } else {
    const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
    start_sample = elapsed.start;
    end_sample = elapsed.end;
    auto anchor = snapshot.tempo.sample_to_tick(start_sample, snapshot.sample_rate);
    if (!anchor)
      return Result::failure(anchor.error());
    start_tick = anchor.value();
  }
  if (target.value() < start_sample || target.value() >= end_sample || tick < start_tick)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Channel point must be inside its audio clip"});
  return Result::success(timebase == project::AutomationTimebase::musical
                             ? tick.value - start_tick.value
                             : target.value().value - start_sample.value);
}

[[nodiscard]] foundation::Result<std::int64_t>
clip_channel_coordinate_at_fraction(const project::Snapshot& snapshot, const project::Clip& clip,
                                    project::AutomationTimebase timebase, double fraction) {
  using Result = foundation::Result<std::int64_t>;
  if (!std::isfinite(fraction) || fraction < 0.0 || fraction >= 1.0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Channel point fraction must be inside clip"});
  if (const auto* musical = std::get_if<project::MusicalInterval>(&clip.placement)) {
    const auto duration = musical->end.value - musical->start.value;
    if (duration <= 0)
      return Result::failure({foundation::ErrorCode::invalid_argument, "Empty audio clip"});
    const auto offset = static_cast<std::int64_t>(std::min<long double>(
        duration - 1, std::floor(static_cast<long double>(fraction) * duration)));
    return clip_channel_coordinate(snapshot, clip, timebase, {musical->start.value + offset});
  }
  const auto& elapsed = std::get<project::ElapsedInterval>(clip.placement);
  const auto duration = elapsed.end.value - elapsed.start.value;
  if (duration <= 0)
    return Result::failure({foundation::ErrorCode::invalid_argument, "Empty audio clip"});
  const auto offset = static_cast<std::int64_t>(std::min<long double>(
      duration - 1, std::floor(static_cast<long double>(fraction) * duration)));
  if (timebase == project::AutomationTimebase::elapsed)
    return Result::success(offset);
  auto tick = snapshot.tempo.sample_to_tick({elapsed.start.value + offset}, snapshot.sample_rate);
  return tick ? clip_channel_coordinate(snapshot, clip, timebase, tick.value())
              : Result::failure(tick.error());
}

[[nodiscard]] foundation::Result<project::AutomationLane>
convert_automation_timebase(const project::Snapshot& state, const project::AutomationLane& lane,
                            const project::Placement* placement) {
  using foundation::ErrorCode;
  using Result = foundation::Result<project::AutomationLane>;
  if (lane.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure({ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  std::int64_t tick_anchor = 0;
  std::int64_t sample_anchor = 0;
  if (placement != nullptr) {
    if (const auto* musical = std::get_if<project::MusicalInterval>(placement)) {
      tick_anchor = musical->start.value;
      auto sample = state.tempo.tick_to_sample(musical->start, state.sample_rate);
      if (!sample)
        return Result::failure(sample.error());
      sample_anchor = sample.value().value;
    } else {
      const auto& elapsed = std::get<project::ElapsedInterval>(*placement);
      sample_anchor = elapsed.start.value;
      auto tick = state.tempo.sample_to_tick(elapsed.start, state.sample_rate);
      if (!tick)
        return Result::failure(tick.error());
      tick_anchor = tick.value().value;
    }
  }
  auto replacement = lane;
  replacement.timebase = lane.timebase == project::AutomationTimebase::musical
                             ? project::AutomationTimebase::elapsed
                             : project::AutomationTimebase::musical;
  std::int64_t previous = -1;
  for (auto& point : replacement.points) {
    if (point.coordinate < 0)
      return Result::failure({ErrorCode::invalid_argument, "Invalid automation point coordinate"});
    std::int64_t converted = 0;
    if (lane.timebase == project::AutomationTimebase::musical) {
      if (point.coordinate > std::numeric_limits<std::int64_t>::max() - tick_anchor)
        return Result::failure({ErrorCode::resource_exhausted, "Automation tick overflow"});
      auto sample = state.tempo.tick_to_sample({tick_anchor + point.coordinate}, state.sample_rate);
      if (!sample)
        return Result::failure(sample.error());
      if (sample.value().value < sample_anchor)
        return Result::failure({ErrorCode::unsupported, "Automation point precedes clip anchor"});
      converted = sample.value().value - sample_anchor;
    } else {
      if (point.coordinate > std::numeric_limits<std::int64_t>::max() - sample_anchor)
        return Result::failure({ErrorCode::resource_exhausted, "Automation sample overflow"});
      const timeline::SamplePosition absolute{sample_anchor + point.coordinate};
      auto tick = state.tempo.sample_to_tick(absolute, state.sample_rate);
      if (!tick)
        return Result::failure(tick.error());
      auto round_trip = state.tempo.tick_to_sample(tick.value(), state.sample_rate);
      if (!round_trip || round_trip.value() != absolute)
        return Result::failure(
            {ErrorCode::unsupported, "Automation sample has no exact musical tick"});
      if (tick.value().value < tick_anchor)
        return Result::failure({ErrorCode::unsupported, "Automation point precedes clip anchor"});
      converted = tick.value().value - tick_anchor;
    }
    if (converted <= previous)
      return Result::failure(
          {ErrorCode::unsupported, "Automation points collide in the target timebase"});
    point.coordinate = converted;
    previous = converted;
  }
  ++replacement.settings_generation;
  return Result::success(std::move(replacement));
}

[[nodiscard]] foundation::Error automation_error(foundation::ErrorCode code, const char* message) {
  return {code, message};
}

// The lane with `id` on a track, with its owner.
struct TrackLane {
  const project::Track* track;
  const project::AutomationLane* lane;
};
[[nodiscard]] std::optional<TrackLane> find_track_lane(const project::Snapshot& snapshot,
                                                       project::AutomationLaneId id) {
  for (const auto& track : snapshot.tracks) {
    const auto lane = std::ranges::find(track.automation, id, &project::AutomationLane::id);
    if (lane != track.automation.end())
      return TrackLane{&track, &*lane};
  }
  return std::nullopt;
}

[[nodiscard]] bool fits_domain(const project::AutomationDomain& domain, double value) noexcept {
  return std::isfinite(value) && value >= domain.minimum && value <= domain.maximum &&
         (!domain.discrete || value == std::round(value));
}

[[nodiscard]] project::AutomationShape
first_shape(const project::AutomationDomain& domain) noexcept {
  // Stepped (discrete) lanes hold between points.
  return domain.discrete ? project::AutomationShape::hold : project::AutomationShape::linear;
}

} // namespace

foundation::Result<project::AutomationPointId>
AutomationService::set_audio_clip_channel_level_point(project::ClipId clip_id,
                                                      std::int64_t zero_based_bar, double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (zero_based_bar < 0 || zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !std::isfinite(value) || value < 0.0 || value > 4.0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid clip channel bar or level"});
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick)
    return Result::failure(tick.error());
  const auto* lane = find_clip_channel_lane(*owner);
  const auto timebase =
      lane == nullptr ? std::holds_alternative<project::MusicalInterval>(owner->clip->placement)
                            ? project::AutomationTimebase::musical
                            : project::AutomationTimebase::elapsed
                      : lane->timebase;
  auto coordinate = clip_channel_coordinate(snapshot(), *owner->clip, timebase, tick.value());
  if (!coordinate)
    return Result::failure(coordinate.error());
  return set_audio_clip_channel_level_point_at_coordinate(clip_id, coordinate.value(), value);
}

foundation::Result<project::AutomationPointId>
AutomationService::set_audio_clip_channel_level_point_at_fraction(project::ClipId clip_id,
                                                                  ClipFraction fraction,
                                                                  double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (!std::isfinite(value) || value < 0.0 || value > 4.0)
    return Result::failure({foundation::ErrorCode::invalid_argument, "Invalid clip channel level"});
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  const auto timebase =
      lane == nullptr ? std::holds_alternative<project::MusicalInterval>(owner->clip->placement)
                            ? project::AutomationTimebase::musical
                            : project::AutomationTimebase::elapsed
                      : lane->timebase;
  auto coordinate =
      clip_channel_coordinate_at_fraction(snapshot(), *owner->clip, timebase, fraction.value);
  if (!coordinate)
    return Result::failure(coordinate.error());
  return set_audio_clip_channel_level_point_at_coordinate(clip_id, coordinate.value(), value);
}

foundation::Result<project::AutomationPointId>
AutomationService::set_audio_clip_channel_level_point_at_coordinate(project::ClipId clip_id,
                                                                    std::int64_t coordinate,
                                                                    double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  const auto target = project::TrackControlTarget{owner->track->id,
                                                  project::TrackControlTarget::Key::channel_level};
  const auto timebase =
      lane == nullptr ? std::holds_alternative<project::MusicalInterval>(owner->clip->placement)
                            ? project::AutomationTimebase::musical
                            : project::AutomationTimebase::elapsed
                      : lane->timebase;
  if (lane == nullptr) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id)
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    project::AutomationLane created{
        lane_id.value(),
        target,
        {project::AutomationRepresentation::physical, "mixer.channel_level", "linear_gain", 0.0,
         4.0, owner->track->level, false, 0, project::AutomationScale::decibel},
        timebase,
        {{point_id.value(), coordinate, value, project::AutomationShape::linear}},
        1};
    auto applied = model().apply({project::AddAutomationLane{clip_id, std::move(created)}});
    return applied ? Result::success(point_id.value()) : Result::failure(applied.error());
  }
  return upsert_lane_point(model(), *lane, coordinate, value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void>
AutomationService::move_audio_clip_channel_level_point(project::ClipId clip_id,
                                                       project::AutomationPointId point_id,
                                                       ClipFraction fraction, double value) {
  using Result = foundation::Result<void>;
  if (!std::isfinite(value) || value < 0.0 || value > 4.0)
    return Result::failure({foundation::ErrorCode::invalid_argument, "Invalid clip channel level"});
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Clip channel lane not found"});
  auto coordinate =
      clip_channel_coordinate_at_fraction(snapshot(), *owner->clip, lane->timebase, fraction.value);
  if (!coordinate)
    return Result::failure(coordinate.error());
  auto replacement = *lane;
  const auto point = std::ranges::find(replacement.points, point_id, &project::AutomationPoint::id);
  if (point == replacement.points.end())
    return Result::failure({foundation::ErrorCode::not_found, "Clip channel point not found"});
  if (point->coordinate == coordinate.value() && point->value == value)
    return Result::success();
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  if (std::ranges::any_of(replacement.points, [coordinate, point_id](const auto& other) {
        return other.id != point_id && other.coordinate == coordinate.value();
      }))
    return Result::failure(
        {foundation::ErrorCode::conflict, "Another point already occupies that clip position"});
  point->coordinate = coordinate.value();
  point->value = value;
  std::ranges::sort(replacement.points, {}, &project::AutomationPoint::coordinate);
  ++replacement.settings_generation;
  auto applied = model().apply({project::ReplaceAutomationLane{lane->id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> AutomationService::set_audio_clip_channel_level_value(
    project::ClipId clip_id, project::AutomationPointId point_id, double value) {
  using Result = foundation::Result<void>;
  if (!std::isfinite(value) || value < 0.0 || value > 4.0)
    return Result::failure({foundation::ErrorCode::invalid_argument, "Invalid clip channel level"});
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Clip channel lane not found"});
  return change_lane_point_value(model(), *lane, point_id, value);
}

foundation::Result<void>
AutomationService::remove_audio_clip_channel_level_point(project::ClipId clip_id,
                                                         project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Clip channel lane not found"});
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void>
AutomationService::remove_audio_clip_channel_level_lane(project::ClipId clip_id) {
  using Result = foundation::Result<void>;
  const auto owner = find_audio_clip(snapshot(), clip_id);
  if (!owner)
    return Result::failure({foundation::ErrorCode::not_found, "Audio clip not found"});
  const auto* lane = find_clip_channel_lane(*owner);
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Clip channel lane not found"});
  return erase_lane(model(), *lane);
}

foundation::Result<void>
AutomationService::toggle_automation_lane_timebase(project::AutomationLaneId id) {
  const auto& state = snapshot();
  const auto apply = [&](const project::AutomationLane& lane,
                         const project::Placement* placement) -> foundation::Result<void> {
    auto replacement = convert_automation_timebase(state, lane, placement);
    if (!replacement)
      return foundation::Result<void>::failure(replacement.error());
    auto changed =
        model().apply({project::ReplaceAutomationLane{id, std::move(replacement).value()}});
    return changed ? foundation::Result<void>::success()
                   : foundation::Result<void>::failure(changed.error());
  };
  for (const auto& track : state.tracks) {
    for (const auto& lane : track.automation) {
      if (lane.id == id)
        return apply(lane, nullptr);
    }
    for (const auto& clip : track.clips) {
      for (const auto& lane : clip.automation) {
        if (lane.id == id)
          return apply(lane, &clip.placement);
      }
    }
    for (const auto& clip : track.midi_clips) {
      for (const auto& lane : clip.automation) {
        if (lane.id == id)
          return apply(lane, &clip.placement);
      }
    }
  }
  return foundation::Result<void>::failure(
      {foundation::ErrorCode::not_found, "Automation lane not found"});
}

foundation::Result<void> AutomationService::set_automation_segment_curve(
    project::AutomationLaneId lane_id, project::AutomationPointId point_id,
    project::AutomationShape shape, project::AutomationHandles handles) {
  using Result = foundation::Result<void>;
  const auto& state = snapshot();
  const project::AutomationLane* lane = nullptr;
  for (const auto& track : state.tracks) {
    const auto find_in = [&](const auto& lanes) {
      return std::ranges::find(lanes, lane_id, &project::AutomationLane::id);
    };
    if (const auto found = find_in(track.automation); found != track.automation.end()) {
      lane = &*found;
      break;
    }
    for (const auto& clip : track.clips) {
      if (const auto found = find_in(clip.automation); found != clip.automation.end()) {
        lane = &*found;
        break;
      }
    }
    if (lane != nullptr)
      break;
    for (const auto& clip : track.midi_clips) {
      if (const auto found = find_in(clip.automation); found != clip.automation.end()) {
        lane = &*found;
        break;
      }
    }
    if (lane != nullptr)
      break;
  }
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Automation lane not found"});
  auto replacement = *lane;
  const auto point = std::ranges::find(replacement.points, point_id, &project::AutomationPoint::id);
  if (point == replacement.points.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation point not found"});
  if (point + 1 == replacement.points.end())
    return Result::failure({foundation::ErrorCode::invalid_argument,
                            "Final automation point has no following segment"});
  if (shape > project::AutomationShape::bezier ||
      (lane->domain.discrete && shape != project::AutomationShape::hold) ||
      !std::isfinite(handles.x1) || !std::isfinite(handles.x2) || !std::isfinite(handles.y1) ||
      !std::isfinite(handles.y2) || handles.x1 < 0.0 || handles.x1 > handles.x2 ||
      handles.x2 > 1.0 || handles.y1 < 0.0 || handles.y1 > 1.0 || handles.y2 < 0.0 ||
      handles.y2 > 1.0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid automation segment curve"});
  }
  if (point->to_next == shape && point->handles == handles)
    return Result::success();
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  point->to_next = shape;
  point->handles = handles;
  ++replacement.settings_generation;
  auto applied = model().apply({project::ReplaceAutomationLane{lane_id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<project::AutomationDomain>
AutomationService::automation_domain(project::TrackId track_id,
                                     const project::AutomationTarget& target) const {
  using Result = foundation::Result<project::AutomationDomain>;
  const auto& state = snapshot();
  const auto track = std::ranges::find(state.tracks, track_id, &project::Track::id);
  if (track == state.tracks.end())
    return Result::failure(
        automation_error(foundation::ErrorCode::not_found, "Automation track not found"));
  return std::visit(
      [&](const auto& item) -> Result {
        using Target = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Target, project::TrackControlTarget>) {
          if (item.track != track_id || !project::is_channel_control(item.key))
            return Result::failure(automation_error(foundation::ErrorCode::unsupported,
                                                    "Not a channel control of this track"));
          const auto domain = project::channel_control_domain(item.key);
          using Key = project::TrackControlTarget::Key;
          const double current = item.key == Key::channel_pan    ? track->pan
                                 : item.key == Key::channel_mute ? (track->muted ? 1.0 : 0.0)
                                                                 : track->level;
          return Result::success({project::AutomationRepresentation::physical, domain.semantic_key,
                                  domain.unit, domain.minimum, domain.maximum, current,
                                  domain.discrete, domain.step_count, domain.scale});
        } else if constexpr (std::is_same_v<Target, project::SendGainTarget>) {
          const auto route = std::ranges::find(track->sends, item.send, &project::SendRoute::id);
          if (route == track->sends.end())
            return Result::failure(
                automation_error(foundation::ErrorCode::not_found, "Send is not on this track"));
          return Result::success({project::AutomationRepresentation::physical, "mixer.send_gain",
                                  "linear_gain", 0.0, 4.0, route->gain, false, 0,
                                  project::AutomationScale::decibel});
        } else if constexpr (std::is_same_v<Target, project::NativeEffectParameterTarget>) {
          auto found = native_effect_slot(state, project::SignalPathId{track_id}, item.slot);
          if (!found)
            return Result::failure(found.error());
          return native_inserts::native_effect_automation_domain(*found.value().resolved.engine,
                                                                 item.parameter_id);
        } else if constexpr (std::is_same_v<Target, project::NativeParameterTarget>) {
          if (item.track != track_id)
            return Result::failure(automation_error(foundation::ErrorCode::unsupported,
                                                    "Synth parameter is not on this track"));
          auto found = native_track(state, track_id);
          if (!found)
            return Result::failure(found.error());
          return native_dsp::synth::native_automation_domain(*found.value().resolved.engine,
                                                             item.parameter_id);
        } else if constexpr (std::is_same_v<Target, project::MacroTarget>) {
          if (item.track != track_id || item.index >= track->macros.size())
            return Result::failure(
                automation_error(foundation::ErrorCode::not_found, "Macro is not on this track"));
          return Result::success(project::macro_automation_domain(track->macros[item.index].value));
        } else {
          const auto* plugin = saved_automatable_plugin(*track, item.plugin);
          if (plugin == nullptr)
            return Result::failure(automation_error(foundation::ErrorCode::unavailable,
                                                    "VST3 automation plugin is not enabled"));
          const auto reference = std::ranges::find(plugin->parameters, item.parameter_id,
                                                   &project::Vst3ParameterReference::id);
          if (reference == plugin->parameters.end())
            return Result::failure(automation_error(foundation::ErrorCode::unavailable,
                                                    "VST3 parameter has no saved value reference"));
          auto semantic_key =
              "vst3.parameter." + plugin->class_id + "." + std::to_string(item.parameter_id);
          if (semantic_key.size() > 128)
            return Result::failure(automation_error(foundation::ErrorCode::invalid_argument,
                                                    "VST3 parameter identity is too long"));
          return Result::success({project::AutomationRepresentation::vst3_normalized,
                                  std::move(semantic_key), "normalized", 0.0, 1.0,
                                  reference->normalized_value, false, 0});
        }
      },
      target);
}

foundation::Result<project::AutomationPointId>
AutomationService::add_automation_point(project::TrackId track_id,
                                        const project::AutomationTarget& target, std::int64_t tick,
                                        double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (tick < 0)
    return Result::failure(
        automation_error(foundation::ErrorCode::invalid_argument, "Automation tick is negative"));
  auto domain = automation_domain(track_id, target);
  if (!domain)
    return Result::failure(domain.error());
  if (!fits_domain(domain.value(), value))
    return Result::failure(automation_error(foundation::ErrorCode::invalid_argument,
                                            "Automation value is out of range"));
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  const auto* existing = find_lane(*track, target);
  const auto next_point = [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  };
  if (existing != nullptr) {
    auto coordinate = point_coordinate(snapshot(), *existing, timeline::TickPosition{tick});
    if (!coordinate)
      return Result::failure(coordinate.error());
    return upsert_lane_point(model(), *existing, coordinate.value(), value, next_point);
  }
  const auto [lane_number, point_number] = next_automation_numbers();
  auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
  auto point_id = numbered_id<project::AutomationPointId>(point_number);
  if (!lane_id || !point_id)
    return Result::failure(!lane_id ? lane_id.error() : point_id.error());
  const auto shape = first_shape(domain.value());
  project::AutomationLane lane{lane_id.value(),
                               target,
                               std::move(domain).value(),
                               project::AutomationTimebase::musical,
                               {{point_id.value(), tick, value, shape}},
                               1};
  auto applied = model().apply({project::AddAutomationLane{track_id, std::move(lane)}});
  if (!applied)
    return Result::failure(applied.error());
  return Result::success(point_id.value());
}

foundation::Result<void>
AutomationService::move_automation_points(project::AutomationLaneId lane_id,
                                          std::span<const AutomationPointMove> moves) {
  using Result = foundation::Result<void>;
  const auto found = find_track_lane(snapshot(), lane_id);
  if (!found)
    return Result::failure(
        automation_error(foundation::ErrorCode::not_found, "Automation lane not found"));
  auto replacement = *found->lane;
  bool changed = false;
  for (const auto& move : moves) {
    if (move.tick < 0 || !fits_domain(replacement.domain, move.value))
      return Result::failure(automation_error(foundation::ErrorCode::invalid_argument,
                                              "Automation point move is out of range"));
    const auto point =
        std::ranges::find(replacement.points, move.point, &project::AutomationPoint::id);
    if (point == replacement.points.end())
      return Result::failure(
          automation_error(foundation::ErrorCode::not_found, "Automation point not found"));
    auto coordinate = point_coordinate(snapshot(), replacement, timeline::TickPosition{move.tick});
    if (!coordinate)
      return Result::failure(coordinate.error());
    changed = changed || point->coordinate != coordinate.value() || point->value != move.value;
    point->coordinate = coordinate.value();
    point->value = move.value;
  }
  if (!changed)
    return Result::success();
  std::ranges::stable_sort(replacement.points, {}, &project::AutomationPoint::coordinate);
  if (std::ranges::adjacent_find(replacement.points, {}, &project::AutomationPoint::coordinate) !=
      replacement.points.end())
    return Result::failure(automation_error(foundation::ErrorCode::conflict,
                                            "Two automation points would share one position"));
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(automation_error(foundation::ErrorCode::resource_exhausted,
                                            "Automation lane generation exhausted"));
  ++replacement.settings_generation;
  auto applied = model().apply({project::ReplaceAutomationLane{lane_id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
AutomationService::remove_automation_points(project::AutomationLaneId lane_id,
                                            std::span<const project::AutomationPointId> points) {
  using Result = foundation::Result<void>;
  const auto found = find_track_lane(snapshot(), lane_id);
  if (!found)
    return Result::failure(
        automation_error(foundation::ErrorCode::not_found, "Automation lane not found"));
  auto replacement = *found->lane;
  const auto removed = std::erase_if(replacement.points, [&](const auto& point) {
    return std::ranges::find(points, point.id) != points.end();
  });
  if (removed == 0)
    return Result::success();
  if (replacement.points.empty())
    return erase_lane(model(), *found->lane);
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(automation_error(foundation::ErrorCode::resource_exhausted,
                                            "Automation lane generation exhausted"));
  ++replacement.settings_generation;
  auto applied = model().apply({project::ReplaceAutomationLane{lane_id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void> AutomationService::draw_automation(
    project::TrackId track_id, const project::AutomationTarget& target, std::int64_t from_tick,
    std::int64_t to_tick, std::span<const AutomationDrawPoint> points) {
  using Result = foundation::Result<void>;
  if (from_tick < 0 || to_tick < from_tick || points.empty() ||
      std::ranges::any_of(
          points,
          [&](const auto& point) { return point.tick < from_tick || point.tick > to_tick; }) ||
      std::ranges::adjacent_find(points, std::ranges::greater_equal{},
                                 &AutomationDrawPoint::tick) != points.end())
    return Result::failure(automation_error(foundation::ErrorCode::invalid_argument,
                                            "Drawn automation is out of range or unordered"));
  auto domain = automation_domain(track_id, target);
  if (!domain)
    return Result::failure(domain.error());
  if (!std::ranges::all_of(
          points, [&](const auto& point) { return fits_domain(domain.value(), point.value); }))
    return Result::failure(automation_error(foundation::ErrorCode::invalid_argument,
                                            "Drawn automation value is out of range"));
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  const auto* existing = find_lane(*track, target);
  const auto [lane_number, point_number] = next_automation_numbers();
  auto new_lane_id = numbered_id<project::AutomationLaneId>(lane_number);
  if (existing == nullptr && !new_lane_id)
    return Result::failure(new_lane_id.error());
  project::AutomationLane lane = existing != nullptr
                                     ? *existing
                                     : project::AutomationLane{new_lane_id.value(),
                                                               target,
                                                               domain.value(),
                                                               project::AutomationTimebase::musical,
                                                               {},
                                                               1};
  auto from = point_coordinate(snapshot(), lane, timeline::TickPosition{from_tick});
  auto to = point_coordinate(snapshot(), lane, timeline::TickPosition{to_tick});
  if (!from || !to)
    return Result::failure(!from ? from.error() : to.error());
  std::erase_if(lane.points, [&](const auto& point) {
    return point.coordinate >= from.value() && point.coordinate <= to.value();
  });
  const auto shape = first_shape(domain.value());
  auto number = point_number;
  for (const auto& drawn : points) {
    auto id = numbered_id<project::AutomationPointId>(number++);
    if (!id)
      return Result::failure(id.error());
    auto coordinate = point_coordinate(snapshot(), lane, timeline::TickPosition{drawn.tick});
    if (!coordinate)
      return Result::failure(coordinate.error());
    lane.points.push_back({id.value(), coordinate.value(), drawn.value, shape});
  }
  std::ranges::stable_sort(lane.points, {}, &project::AutomationPoint::coordinate);
  if (std::ranges::adjacent_find(lane.points, {}, &project::AutomationPoint::coordinate) !=
      lane.points.end())
    return Result::failure(automation_error(foundation::ErrorCode::conflict,
                                            "Drawn automation points share one position"));
  if (lane.points.size() > project::max_automation_points_per_lane)
    return Result::failure(automation_error(foundation::ErrorCode::resource_exhausted,
                                            "Automation point limit exceeded"));
  if (existing == nullptr) {
    auto applied = model().apply({project::AddAutomationLane{track_id, std::move(lane)}});
    return applied ? Result::success() : Result::failure(applied.error());
  }
  if (lane.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(automation_error(foundation::ErrorCode::resource_exhausted,
                                            "Automation lane generation exhausted"));
  ++lane.settings_generation;
  const auto id = lane.id;
  auto applied = model().apply({project::ReplaceAutomationLane{id, std::move(lane)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

} // namespace oscillate::application
