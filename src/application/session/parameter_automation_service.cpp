#include "oscillate/application/session/parameter_automation_service.hpp"

#include "edit_support.hpp"
#include "midi_clips.hpp"
#include "oscillate/application/key_cache.hpp"
#include "oscillate/application/midi_note_tools.hpp"
#include "oscillate/application/tempo_cache.hpp"
#include "oscillate/midi_file/smf.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "session_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace oscillate::application {
namespace {

using ChannelKey = project::TrackControlTarget::Key;
using detail::find_midi_clip;
using detail::midi_anchor_tick;

[[nodiscard]] bool send_gain_lane(const project::AutomationLane& lane, project::SendId send) {
  const auto* target = std::get_if<project::SendGainTarget>(&lane.target);
  return target != nullptr && target->send == send;
}

[[nodiscard]] const project::Track* send_source(const project::Snapshot& snapshot,
                                                project::SendId send) {
  const auto track = std::ranges::find_if(snapshot.tracks, [send](const auto& candidate) {
    return std::ranges::any_of(candidate.sends,
                               [send](const auto& route) { return route.id == send; });
  });
  return track == snapshot.tracks.end() ? nullptr : &*track;
}

project::NativeParameterTarget native_target(project::TrackId track, std::string_view parameter) {
  return {track, std::string(parameter)};
}

project::NativeEffectParameterTarget effect_target(project::InsertSlotId slot,
                                                   std::string_view parameter) {
  return {slot, std::string(parameter)};
}

[[nodiscard]] bool valid_channel_value(ChannelKey key, double value) noexcept {
  const auto domain = project::channel_control_domain(key);
  return std::isfinite(value) && value >= domain.minimum && value <= domain.maximum &&
         (!domain.discrete || value == std::round(value));
}

} // namespace

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_native_automation_point(project::TrackId track_id,
                                                        std::string_view parameter,
                                                        std::int64_t zero_based_bar, double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (zero_based_bar < 0 || zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !std::isfinite(value)) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid synth automation bar or value"});
  }
  auto found = native_track(snapshot(), track_id);
  if (!found) {
    return Result::failure(found.error());
  }
  auto domain = native_inserts::native_automation_domain(*found.value().resolved.engine, parameter);
  if (!domain) {
    return Result::failure(domain.error());
  }
  if (value < domain.value().minimum || value > domain.value().maximum ||
      (domain.value().discrete && value != std::round(value))) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Synth automation value is out of range"});
  }
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick) {
    return Result::failure(tick.error());
  }
  const auto& track = *found.value().track;
  const auto* existing = find_lane(track, native_target(track_id, parameter));
  const bool stepped = domain.value().discrete;
  if (existing == nullptr) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id) {
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    }
    project::AutomationLane lane{
        lane_id.value(),
        native_target(track_id, parameter),
        std::move(domain).value(),
        project::AutomationTimebase::musical,
        {{point_id.value(), tick.value().value, value,
          stepped ? project::AutomationShape::hold : project::AutomationShape::linear}},
        1};
    auto applied = model().apply({project::AddAutomationLane{track_id, std::move(lane)}});
    if (!applied) {
      return Result::failure(applied.error());
    }
    return Result::success(point_id.value());
  }
  auto coordinate = point_coordinate(snapshot(), *existing, tick.value());
  if (!coordinate) {
    return Result::failure(coordinate.error());
  }
  return upsert_lane_point(model(), *existing, coordinate.value(), value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void> ParameterAutomationService::set_native_automation_value(
    project::TrackId track_id, std::string_view parameter, project::AutomationPointId point_id,
    double value) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  }
  const auto* lane = find_lane(*track, native_target(track_id, parameter));
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Synth automation lane not found"});
  }
  if (!std::isfinite(value) || value < lane->domain.minimum || value > lane->domain.maximum ||
      (lane->domain.discrete && value != std::round(value))) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Synth automation value is out of range"});
  }
  return change_lane_point_value(model(), *lane, point_id, value);
}

foundation::Result<void> ParameterAutomationService::remove_native_automation_point(
    project::TrackId track_id, std::string_view parameter, project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  }
  const auto* lane = find_lane(*track, native_target(track_id, parameter));
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Synth automation lane not found"});
  }
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void>
ParameterAutomationService::remove_native_automation_lane(project::TrackId track_id,
                                                          std::string_view parameter) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end()) {
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  }
  const auto* lane = find_lane(*track, native_target(track_id, parameter));
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Synth automation lane not found"});
  }
  return erase_lane(model(), *lane);
}

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_effect_automation_point(project::TrackId track_id,
                                                        project::InsertSlotId slot,
                                                        std::string_view parameter,
                                                        std::int64_t zero_based_bar, double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (zero_based_bar < 0 || zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !std::isfinite(value)) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid effect automation bar or value"});
  }
  auto found = native_effect_slot(snapshot(), track_id, slot);
  if (!found)
    return Result::failure(found.error());
  auto domain =
      native_inserts::native_effect_automation_domain(*found.value().resolved.engine, parameter);
  if (!domain)
    return Result::failure(domain.error());
  if (value < domain.value().minimum || value > domain.value().maximum ||
      (domain.value().discrete && value != std::round(value))) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Effect automation value is out of range"});
  }
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick)
    return Result::failure(tick.error());
  const auto& track = *found.value().track;
  const auto* existing = find_lane(track, effect_target(slot, parameter));
  const bool stepped = domain.value().discrete;
  if (existing == nullptr) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id)
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    project::AutomationLane lane{
        lane_id.value(),
        effect_target(slot, parameter),
        std::move(domain).value(),
        project::AutomationTimebase::musical,
        {{point_id.value(), tick.value().value, value,
          stepped ? project::AutomationShape::hold : project::AutomationShape::linear}},
        1};
    auto applied = model().apply({project::AddAutomationLane{track_id, std::move(lane)}});
    if (!applied)
      return Result::failure(applied.error());
    return Result::success(point_id.value());
  }
  auto coordinate = point_coordinate(snapshot(), *existing, tick.value());
  if (!coordinate)
    return Result::failure(coordinate.error());
  return upsert_lane_point(model(), *existing, coordinate.value(), value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void> ParameterAutomationService::set_effect_automation_value(
    project::TrackId track_id, project::InsertSlotId slot, std::string_view parameter,
    project::AutomationPointId point_id, double value) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, effect_target(slot, parameter));
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Effect automation lane not found"});
  if (!std::isfinite(value) || value < lane->domain.minimum || value > lane->domain.maximum ||
      (lane->domain.discrete && value != std::round(value))) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Effect automation value is out of range"});
  }
  return change_lane_point_value(model(), *lane, point_id, value);
}

foundation::Result<void> ParameterAutomationService::remove_effect_automation_point(
    project::TrackId track_id, project::InsertSlotId slot, std::string_view parameter,
    project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, effect_target(slot, parameter));
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Effect automation lane not found"});
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void> ParameterAutomationService::remove_effect_automation_lane(
    project::TrackId track_id, project::InsertSlotId slot, std::string_view parameter) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, effect_target(slot, parameter));
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Effect automation lane not found"});
  return erase_lane(model(), *lane);
}

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_channel_control_point(project::TrackId track_id, ChannelKey key,
                                                      std::int64_t zero_based_bar, double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (!project::is_channel_control(key) || zero_based_bar < 0 ||
      zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !valid_channel_value(key, value))
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid channel automation bar or value"});
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick)
    return Result::failure(tick.error());
  const auto target = project::TrackControlTarget{track_id, key};
  const auto* existing = find_lane(*track, target);
  if (existing == nullptr) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id)
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    const auto domain = project::channel_control_domain(key);
    const double current = key == ChannelKey::channel_pan    ? track->pan
                           : key == ChannelKey::channel_mute ? (track->muted ? 1.0 : 0.0)
                                                             : track->level;
    project::AutomationLane lane{
        lane_id.value(),
        target,
        {project::AutomationRepresentation::physical, domain.semantic_key, domain.unit,
         domain.minimum, domain.maximum, current, domain.discrete, domain.step_count, domain.scale},
        project::AutomationTimebase::musical,
        {{point_id.value(), tick.value().value, value,
          domain.discrete ? project::AutomationShape::hold : project::AutomationShape::linear}},
        1};
    auto applied = model().apply({project::AddAutomationLane{track_id, std::move(lane)}});
    if (!applied)
      return Result::failure(applied.error());
    return Result::success(point_id.value());
  }
  auto coordinate = point_coordinate(snapshot(), *existing, tick.value());
  if (!coordinate)
    return Result::failure(coordinate.error());
  return upsert_lane_point(model(), *existing, coordinate.value(), value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void> ParameterAutomationService::set_channel_control_value(
    project::TrackId track_id, ChannelKey key, project::AutomationPointId point_id, double value) {
  using Result = foundation::Result<void>;
  if (!project::is_channel_control(key) || !valid_channel_value(key, value))
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid channel automation value"});
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::TrackControlTarget{track_id, key});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Channel automation lane not found"});
  return change_lane_point_value(model(), *lane, point_id, value);
}

foundation::Result<void>
ParameterAutomationService::move_channel_control_point(project::TrackId track_id, ChannelKey key,
                                                       project::AutomationPointId point_id,
                                                       std::int64_t zero_based_bar, double value) {
  using Result = foundation::Result<void>;
  if (!project::is_channel_control(key) || zero_based_bar < 0 ||
      zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !valid_channel_value(key, value))
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid channel automation bar or value"});
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::TrackControlTarget{track_id, key});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Channel automation lane not found"});
  const auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick)
    return Result::failure(tick.error());
  const auto coordinate = point_coordinate(snapshot(), *lane, tick.value());
  if (!coordinate)
    return Result::failure(coordinate.error());
  auto replacement = *lane;
  const auto point = std::ranges::find(replacement.points, point_id, &project::AutomationPoint::id);
  if (point == replacement.points.end())
    return Result::failure(
        {foundation::ErrorCode::not_found, "Channel automation point not found"});
  if (point->coordinate == coordinate.value() && point->value == value)
    return Result::success();
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max())
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  if (std::ranges::any_of(replacement.points, [point_id, &coordinate](const auto& other) {
        return other.id != point_id && other.coordinate == coordinate.value();
      }))
    return Result::failure(
        {foundation::ErrorCode::conflict, "Another channel point occupies that bar"});
  point->coordinate = coordinate.value();
  point->value = value;
  std::ranges::sort(replacement.points, {}, &project::AutomationPoint::coordinate);
  ++replacement.settings_generation;
  const auto applied =
      model().apply({project::ReplaceAutomationLane{lane->id, std::move(replacement)}});
  return applied ? Result::success() : Result::failure(applied.error());
}

foundation::Result<void>
ParameterAutomationService::remove_channel_control_point(project::TrackId track_id, ChannelKey key,
                                                         project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::TrackControlTarget{track_id, key});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Channel automation lane not found"});
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void>
ParameterAutomationService::remove_channel_control_lane(project::TrackId track_id, ChannelKey key) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::TrackControlTarget{track_id, key});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "Channel automation lane not found"});
  return erase_lane(model(), *lane);
}

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_channel_level_automation_point(project::TrackId track_id,
                                                               std::int64_t zero_based_bar,
                                                               double value) {
  return set_channel_control_point(track_id, ChannelKey::channel_level, zero_based_bar, value);
}

foundation::Result<void> ParameterAutomationService::set_channel_level_automation_value(
    project::TrackId track_id, project::AutomationPointId point_id, double value) {
  return set_channel_control_value(track_id, ChannelKey::channel_level, point_id, value);
}

foundation::Result<void> ParameterAutomationService::move_channel_level_automation_point(
    project::TrackId track_id, project::AutomationPointId point_id, std::int64_t zero_based_bar,
    double value) {
  return move_channel_control_point(track_id, ChannelKey::channel_level, point_id, zero_based_bar,
                                    value);
}

foundation::Result<void> ParameterAutomationService::remove_channel_level_automation_point(
    project::TrackId track_id, project::AutomationPointId point_id) {
  return remove_channel_control_point(track_id, ChannelKey::channel_level, point_id);
}

foundation::Result<void>
ParameterAutomationService::remove_channel_level_automation_lane(project::TrackId track_id) {
  return remove_channel_control_lane(track_id, ChannelKey::channel_level);
}

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_vst3_parameter_automation_point(project::TrackId track_id,
                                                                project::PluginInstanceId plugin_id,
                                                                std::uint32_t parameter_id,
                                                                std::int64_t zero_based_bar,
                                                                double normalized_value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (zero_based_bar < 0 || zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !std::isfinite(normalized_value) || normalized_value < 0.0 || normalized_value > 1.0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid VST3 automation bar or value"});
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* plugin = saved_automatable_plugin(*track, plugin_id);
  if (plugin == nullptr)
    return Result::failure(
        {foundation::ErrorCode::unavailable, "VST3 automation plugin is not enabled"});
  const auto reference =
      std::ranges::find(plugin->parameters, parameter_id, &project::Vst3ParameterReference::id);
  if (reference == plugin->parameters.end())
    return Result::failure(
        {foundation::ErrorCode::unavailable, "VST3 parameter has no saved value reference"});
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick)
    return Result::failure(tick.error());
  const project::Vst3ParameterTarget target{plugin_id, parameter_id};
  const auto* lane = find_lane(*track, target);
  if (lane == nullptr) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id)
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    auto semantic_key = "vst3.parameter." + plugin->class_id + "." + std::to_string(parameter_id);
    if (semantic_key.size() > 128)
      return Result::failure(
          {foundation::ErrorCode::invalid_argument, "VST3 parameter identity is too long"});
    project::AutomationLane created{lane_id.value(),
                                    target,
                                    {project::AutomationRepresentation::vst3_normalized,
                                     std::move(semantic_key), "normalized", 0.0, 1.0,
                                     reference->normalized_value, false, 0},
                                    project::AutomationTimebase::musical,
                                    {{point_id.value(), tick.value().value, normalized_value,
                                      project::AutomationShape::linear}},
                                    1};
    auto applied = model().apply({project::AddAutomationLane{track_id, std::move(created)}});
    if (!applied)
      return Result::failure(applied.error());
    return Result::success(point_id.value());
  }
  auto coordinate = point_coordinate(snapshot(), *lane, tick.value());
  if (!coordinate)
    return Result::failure(coordinate.error());
  return upsert_lane_point(model(), *lane, coordinate.value(), normalized_value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void> ParameterAutomationService::set_vst3_parameter_automation_value(
    project::TrackId track_id, project::PluginInstanceId plugin_id, std::uint32_t parameter_id,
    project::AutomationPointId point_id, double normalized_value) {
  using Result = foundation::Result<void>;
  if (!std::isfinite(normalized_value) || normalized_value < 0.0 || normalized_value > 1.0)
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid VST3 automation value"});
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::Vst3ParameterTarget{plugin_id, parameter_id});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "VST3 automation lane not found"});
  return change_lane_point_value(model(), *lane, point_id, normalized_value);
}

foundation::Result<void> ParameterAutomationService::remove_vst3_parameter_automation_point(
    project::TrackId track_id, project::PluginInstanceId plugin_id, std::uint32_t parameter_id,
    project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::Vst3ParameterTarget{plugin_id, parameter_id});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "VST3 automation lane not found"});
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void> ParameterAutomationService::remove_vst3_parameter_automation_lane(
    project::TrackId track_id, project::PluginInstanceId plugin_id, std::uint32_t parameter_id) {
  using Result = foundation::Result<void>;
  const auto track = std::ranges::find(snapshot().tracks, track_id, &project::Track::id);
  if (track == snapshot().tracks.end())
    return Result::failure({foundation::ErrorCode::not_found, "Automation track not found"});
  const auto* lane = find_lane(*track, project::Vst3ParameterTarget{plugin_id, parameter_id});
  if (lane == nullptr)
    return Result::failure({foundation::ErrorCode::not_found, "VST3 automation lane not found"});
  return erase_lane(model(), *lane);
}

foundation::Result<project::AutomationPointId>
ParameterAutomationService::set_send_gain_automation_point(project::SendId send,
                                                           std::int64_t zero_based_bar,
                                                           double value) {
  using Result = foundation::Result<project::AutomationPointId>;
  if (zero_based_bar < 0 || zero_based_bar == std::numeric_limits<std::int64_t>::max() ||
      !std::isfinite(value) || value < 0.0 || value > 4.0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid send automation bar or gain"});
  }
  const auto* track = send_source(snapshot(), send);
  if (track == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send not found"});
  }
  const auto route =
      std::ranges::find_if(track->sends, [send](const auto& item) { return item.id == send; });
  auto tick = snapshot().meter.tick_at({zero_based_bar + 1, 1, 0});
  if (!tick) {
    return Result::failure(tick.error());
  }
  const auto existing = std::ranges::find_if(
      track->automation, [send](const auto& lane) { return send_gain_lane(lane, send); });
  if (existing == track->automation.end()) {
    const auto [lane_number, point_number] = next_automation_numbers();
    auto lane_id = numbered_id<project::AutomationLaneId>(lane_number);
    auto point_id = numbered_id<project::AutomationPointId>(point_number);
    if (!lane_id || !point_id) {
      return Result::failure(!lane_id ? lane_id.error() : point_id.error());
    }
    project::AutomationLane lane{
        lane_id.value(),
        project::SendGainTarget{send},
        {project::AutomationRepresentation::physical, "mixer.send_gain", "linear_gain", 0.0, 4.0,
         route->gain, false, 0, project::AutomationScale::decibel},
        project::AutomationTimebase::musical,
        {{point_id.value(), tick.value().value, value, project::AutomationShape::linear}},
        1};
    auto applied = model().apply({project::AddAutomationLane{track->id, std::move(lane)}});
    if (!applied) {
      return Result::failure(applied.error());
    }
    return Result::success(point_id.value());
  }
  auto coordinate = point_coordinate(snapshot(), *existing, tick.value());
  if (!coordinate) {
    return Result::failure(coordinate.error());
  }
  return upsert_lane_point(model(), *existing, coordinate.value(), value, [this] {
    return numbered_id<project::AutomationPointId>(next_automation_numbers().second);
  });
}

foundation::Result<void> ParameterAutomationService::set_send_gain_automation_value(
    project::SendId send, project::AutomationPointId point_id, double value) {
  using Result = foundation::Result<void>;
  if (!std::isfinite(value) || value < 0.0 || value > 4.0) {
    return Result::failure(
        {foundation::ErrorCode::invalid_argument, "Invalid send automation gain"});
  }
  const auto* track = send_source(snapshot(), send);
  if (track == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send not found"});
  }
  const auto* lane = find_lane(*track, project::SendGainTarget{send});
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send automation lane not found"});
  }
  return change_lane_point_value(model(), *lane, point_id, value);
}

foundation::Result<void>
ParameterAutomationService::remove_send_gain_automation_point(project::SendId send,
                                                              project::AutomationPointId point_id) {
  using Result = foundation::Result<void>;
  const auto* track = send_source(snapshot(), send);
  if (track == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send not found"});
  }
  const auto* lane = find_lane(*track, project::SendGainTarget{send});
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send automation lane not found"});
  }
  return erase_lane_point(model(), *lane, point_id);
}

foundation::Result<void>
ParameterAutomationService::remove_send_gain_automation_lane(project::SendId send) {
  using Result = foundation::Result<void>;
  const auto* track = send_source(snapshot(), send);
  if (track == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send not found"});
  }
  const auto* lane = find_lane(*track, project::SendGainTarget{send});
  if (lane == nullptr) {
    return Result::failure({foundation::ErrorCode::not_found, "Send automation lane not found"});
  }
  return erase_lane(model(), *lane);
}

} // namespace oscillate::application
