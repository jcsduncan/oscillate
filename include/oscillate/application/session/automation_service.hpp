#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

struct ClipFraction {
  double value;
};

class AutomationService : public SessionService {
public:
  explicit AutomationService(SessionState& state) noexcept : SessionService(state) {}

  // Track automation for any target the track owns (its mixer channel, its
  // sends, its built-in or VST3 effects and instrument), at any project tick.
  // A lane is created with its first point, taking the target's domain with
  // its current value as default; removing its last point removes the lane.
  // Each call publishes one undoable revision. Ticks map to an elapsed lane's
  // samples through the tempo map.
  struct AutomationPointMove {
    project::AutomationPointId point;
    std::int64_t tick;
    double value;
  };
  struct AutomationDrawPoint {
    std::int64_t tick;
    double value;
  };

  // Audio clip envelopes use clip-local coordinates and override the owning
  // track's channel level only while the clip is active.
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_audio_clip_channel_level_point(project::ClipId clip, std::int64_t zero_based_bar,
                                     double value);
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_audio_clip_channel_level_point_at_fraction(project::ClipId clip, ClipFraction fraction,
                                                 double value);
  [[nodiscard]] foundation::Result<void>
  move_audio_clip_channel_level_point(project::ClipId clip, project::AutomationPointId point,
                                      ClipFraction fraction, double value);
  [[nodiscard]] foundation::Result<void>
  set_audio_clip_channel_level_value(project::ClipId clip, project::AutomationPointId point,
                                     double value);
  [[nodiscard]] foundation::Result<void>
  remove_audio_clip_channel_level_point(project::ClipId clip, project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void> remove_audio_clip_channel_level_lane(project::ClipId clip);
  // Preserve each point's current audible sample while changing the lane's
  // future tempo anchor. Clip-owned point coordinates remain clip-local.
  [[nodiscard]] foundation::Result<void>
  toggle_automation_lane_timebase(project::AutomationLaneId lane);
  // Edits the segment beginning at `point`; the final point has no segment.
  // One accepted change publishes one undoable lane revision. Handles are
  // retained for every shape so switching back to Bézier restores the edit.
  [[nodiscard]] foundation::Result<void>
  set_automation_segment_curve(project::AutomationLaneId lane, project::AutomationPointId point,
                               project::AutomationShape shape, project::AutomationHandles handles);
  [[nodiscard]] foundation::Result<project::AutomationDomain>
  automation_domain(project::TrackId track, const project::AutomationTarget& target) const;
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  add_automation_point(project::TrackId track, const project::AutomationTarget& target,
                       std::int64_t tick, double value);
  [[nodiscard]] foundation::Result<void>
  move_automation_points(project::AutomationLaneId lane,
                         std::span<const AutomationPointMove> moves);
  [[nodiscard]] foundation::Result<void>
  remove_automation_points(project::AutomationLaneId lane,
                           std::span<const project::AutomationPointId> points);
  // Replaces the lane's points in [from_tick, to_tick] with `points` (ticks
  // inside that range, ascending): a drawn stretch of envelope.
  [[nodiscard]] foundation::Result<void>
  draw_automation(project::TrackId track, const project::AutomationTarget& target,
                  std::int64_t from_tick, std::int64_t to_tick,
                  std::span<const AutomationDrawPoint> points);

private:
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_audio_clip_channel_level_point_at_coordinate(project::ClipId clip, std::int64_t coordinate,
                                                   double value);
};

} // namespace oscillate::application
