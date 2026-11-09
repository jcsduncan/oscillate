#pragma once

#include "oscillate/application/session/effect_service.hpp"
#include "oscillate/foundation/error.hpp"
#include "oscillate/foundation/id.hpp"
#include "oscillate/native_inserts/effect_binding.hpp"
#include "oscillate/native_inserts/instrument_binding.hpp"
#include "oscillate/project/model.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Helpers shared by the ProjectSession services.
namespace oscillate::application {

// Session-numbered IDs: {0, number}. Number zero means the space ran out.
template <typename Id> [[nodiscard]] foundation::Result<Id> numbered_id(std::uint64_t number) {
  if (number == 0)
    return foundation::Result<Id>::failure(
        {foundation::ErrorCode::resource_exhausted, "Project ID space exhausted"});
  return Id::from_bits({0, number});
}

inline constexpr std::string_view repair_in_progress_message =
    "Wait for managed-media repair to finish";

// The refusal for a project edit while a managed-media repair runs.
[[nodiscard]] inline foundation::Error repair_in_progress() {
  return {foundation::ErrorCode::conflict, std::string(repair_in_progress_message)};
}

[[nodiscard]] project::Vst3PluginInstance make_vst3_instance(project::PluginInstanceId id,
                                                             Vst3InsertIdentity identity);

[[nodiscard]] std::uint64_t next_number(std::uint64_t greatest);

[[nodiscard]] const project::AutomationLane* find_lane(const project::Track& track,
                                                       const project::AutomationTarget& target);

[[nodiscard]] const project::Vst3PluginInstance*
saved_automatable_plugin(const project::Track& track, project::PluginInstanceId id);

[[nodiscard]] foundation::Result<std::int64_t> point_coordinate(const project::Snapshot& snapshot,
                                                                const project::AutomationLane& lane,
                                                                timeline::TickPosition tick);

[[nodiscard]] foundation::Result<project::Placement>
convert_clip_timebase(const project::Snapshot& state, const project::Placement& placement);

[[nodiscard]] foundation::Result<void>
assign_fresh_automation_ids(std::vector<project::AutomationLane>& lanes,
                            std::uint64_t current_revision);

struct SplitClipAutomation {
  std::vector<project::AutomationLane> left;
  std::vector<project::AutomationLane> right;
};

[[nodiscard]] foundation::Result<SplitClipAutomation>
split_clip_automation(const project::Snapshot& state,
                      const std::vector<project::AutomationLane>& lanes, std::int64_t tick_offset,
                      std::int64_t sample_offset);

template <typename NewPointId>
foundation::Result<project::AutomationPointId>
upsert_lane_point(project::ProjectModel& model, const project::AutomationLane& lane,
                  std::int64_t coordinate, double value, NewPointId&& new_point_id) {
  using Result = foundation::Result<project::AutomationPointId>;
  auto replacement = lane;
  const auto point = std::ranges::lower_bound(replacement.points, coordinate, {},
                                              &project::AutomationPoint::coordinate);
  if (point != replacement.points.end() && point->coordinate == coordinate &&
      point->value == value) {
    return Result::success(point->id);
  }
  if (replacement.settings_generation == std::numeric_limits<std::uint64_t>::max()) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation lane generation exhausted"});
  }
  const bool replacing = point != replacement.points.end() && point->coordinate == coordinate;
  if (!replacing && replacement.points.size() == project::max_automation_points_per_lane) {
    return Result::failure(
        {foundation::ErrorCode::resource_exhausted, "Automation point limit exceeded"});
  }
  auto point_id = replacing ? Result::success(point->id) : new_point_id();
  if (!point_id) {
    return Result::failure(point_id.error());
  }
  if (replacing) {
    point->value = value;
  } else {
    // Stepped (discrete) lanes hold between points.
    replacement.points.insert(point, {point_id.value(), coordinate, value,
                                      lane.domain.discrete ? project::AutomationShape::hold
                                                           : project::AutomationShape::linear});
  }
  ++replacement.settings_generation;
  auto applied = model.apply({project::ReplaceAutomationLane{lane.id, std::move(replacement)}});
  return applied ? Result::success(point_id.value()) : Result::failure(applied.error());
}

foundation::Result<void> change_lane_point_value(project::ProjectModel& model,
                                                 const project::AutomationLane& lane,
                                                 project::AutomationPointId point_id, double value);

foundation::Result<void> erase_lane_point(project::ProjectModel& model,
                                          const project::AutomationLane& lane,
                                          project::AutomationPointId point_id);

foundation::Result<void> erase_lane(project::ProjectModel& model,
                                    const project::AutomationLane& lane);

foundation::Result<project::Placement> audio_placement_at(const project::Placement& original,
                                                          timeline::TickPosition start,
                                                          const project::Snapshot& snapshot);

// The track's built-in synth and its resolved dense values, or why not.
struct NativeTrack {
  const project::Track* track;
  native_inserts::ResolvedNativeInstrument resolved;
};

foundation::Result<NativeTrack> native_track(const project::Snapshot& snapshot,
                                             project::TrackId track_id);

// A path's built-in effect slot and its resolved dense values, or why not.
struct NativeEffectSlot {
  const project::Track* track; // Null on the master path.
  const project::EffectInsert* slot;
  const project::NativeEffectPatch* patch;
  native_inserts::ResolvedNativeEffect resolved;
};

foundation::Result<NativeEffectSlot> native_effect_slot(const project::Snapshot& snapshot,
                                                        const project::SignalPathId& path,
                                                        project::InsertSlotId slot_id);

// A clip name the model accepts: control characters dropped, cut to the
// limit at a character boundary.
[[nodiscard]] std::string presentable_name(std::string_view name);

// Identities for everything one paste creates. One command makes one
// revision, whose revision-derived namespace stays retired after undo.
struct FreshIdentities {
  std::uint64_t space = 0;
  std::uint64_t next_lane = 1;
  std::uint64_t next_point = 1;
  std::uint64_t next_note = 1;
};

[[nodiscard]] foundation::Result<FreshIdentities> fresh_identities(std::uint64_t revision);

void refresh_lanes(std::vector<project::AutomationLane>& lanes, FreshIdentities& ids);

[[nodiscard]] foundation::Result<void>
done(const foundation::Result<project::AppliedRevision>& applied);

struct LeadShift {
  std::int64_t ticks = 0;   // the bars, in ticks
  std::int64_t samples = 0; // the same span at the opening tempo
};

[[nodiscard]] std::int64_t opening_bar_ticks(const project::Snapshot& state);

// The operations that put `bars` more bars before bar 1: everything on the
// timeline moves later by that span at the opening tempo and meter (the
// opening tempo holds through the new bars, so a ramp keeps its timing),
// except the clips in `placed`, which the caller places itself.
[[nodiscard]] foundation::Result<LeadShift>
lead_bar_operations(const project::Snapshot& state, std::uint32_t bars,
                    std::span<const project::ClipId> placed,
                    std::vector<project::Operation>& operations);

} // namespace oscillate::application
