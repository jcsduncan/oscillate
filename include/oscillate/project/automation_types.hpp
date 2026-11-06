#pragma once

// Saved automation lanes, their targets and value domains.

#include "oscillate/project/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

inline constexpr std::size_t max_automation_lanes_per_track = 128;
inline constexpr std::size_t max_automation_lanes_per_clip = 32;
inline constexpr std::size_t max_automation_lanes_per_project = 65'536;
inline constexpr std::size_t max_automation_points_per_lane = 4'096;
inline constexpr std::size_t max_automation_points_per_project = 1'000'000;

enum class AutomationTimebase : std::uint8_t { musical, elapsed };
enum class AutomationShape : std::uint8_t {
  hold,
  linear,
  quadratic,
  exponential,
  ease_in_out,
  bezier
};
enum class AutomationRepresentation : std::uint8_t { physical, vst3_normalized };
// How a lane's values blend between points: along a straight
// line in the space the lane is drawn in, so a line drawn straight plays
// straight. A gain blends in decibels (silence at the floor), a
// logarithmic parameter in its logarithm, anything else in its own value.
enum class AutomationScale : std::uint8_t { linear, logarithmic, decibel };
// Channel keys drive the routed mixer node: level (linear gain 0..4), pan
// (-1..1, the channel's balance law) and mute (stepped 0/1; a mute lane
// overrides the channel's mute switch).
struct TrackControlTarget {
  TrackId track;
  enum class Key : std::uint8_t { channel_level, channel_pan, channel_mute } key;
  bool operator==(const TrackControlTarget&) const = default;
};
struct Vst3ParameterTarget {
  PluginInstanceId plugin;
  std::uint32_t parameter_id;
  bool operator==(const Vst3ParameterTarget&) const = default;
};
struct SendGainTarget {
  SendId send;
  bool operator==(const SendGainTarget&) const = default;
};
// A parameter of the track's built-in instrument, by the engine's stable id.
// The domain's semantic key repeats the id; values are the engine's plain units.
struct NativeParameterTarget {
  TrackId track;
  std::string parameter_id;
  bool operator==(const NativeParameterTarget&) const = default;
};
// A parameter of a built-in effect (ADR-0040) on the lane owner's track, by
// the effect slot and the engine's stable parameter id. Slot ids are unique
// across the project; the semantic key repeats the parameter id.
struct NativeEffectParameterTarget {
  InsertSlotId slot;
  std::string parameter_id;
  bool operator==(const NativeEffectParameterTarget&) const = default;
};
// One of a track's macro knobs (ADR-0059), by its index in
// Track::macros. The domain is 0..1, linear and continuous, with the semantic
// key "macro". Automation preparation expands a macro lane into one prepared
// lane per mapped parameter, so consumers never see this target.
struct MacroTarget {
  TrackId track;
  std::uint32_t index;
  bool operator==(const MacroTarget&) const = default;
};
using AutomationTarget =
    std::variant<TrackControlTarget, Vst3ParameterTarget, SendGainTarget, NativeParameterTarget,
                 NativeEffectParameterTarget, MacroTarget>;
struct AutomationDomain {
  AutomationRepresentation representation;
  std::string semantic_key;
  std::string unit;
  double minimum;
  double maximum;
  double default_value;
  bool discrete;
  std::uint32_t step_count;
  AutomationScale scale = AutomationScale::linear;
  bool operator==(const AutomationDomain&) const = default;
};
struct AutomationHandles {
  double x1 = 1.0 / 3.0;
  double y1 = 1.0 / 3.0;
  double x2 = 2.0 / 3.0;
  double y2 = 2.0 / 3.0;
  bool operator==(const AutomationHandles&) const = default;
};
struct AutomationPoint {
  AutomationPointId id;
  std::int64_t coordinate;
  double value;
  AutomationShape to_next = AutomationShape::hold;
  AutomationHandles handles{};
  bool operator==(const AutomationPoint&) const = default;
};
struct AutomationLane {
  AutomationLaneId id;
  AutomationTarget target;
  AutomationDomain domain;
  AutomationTimebase timebase = AutomationTimebase::musical;
  std::vector<AutomationPoint> points;
  std::uint64_t settings_generation = 1;
  bool operator==(const AutomationLane&) const = default;
};

} // namespace oscillate::project
