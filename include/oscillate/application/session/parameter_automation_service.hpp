#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class ParameterAutomationService : public SessionService {
public:
  explicit ParameterAutomationService(SessionState& state) noexcept : SessionService(state) {}

  // Track output level follows the same shared curve and bar placement rules
  // as sends. Its values precede stereo balance and all main/send routes.
  // Automation of a channel's level, pan or mute (TrackControlTarget channel
  // keys) on the track lane; mute is stepped 0/1 and overrides the mute switch.
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_channel_control_point(project::TrackId track, project::TrackControlTarget::Key key,
                            std::int64_t zero_based_bar, double value);
  [[nodiscard]] foundation::Result<void>
  set_channel_control_value(project::TrackId track, project::TrackControlTarget::Key key,
                            project::AutomationPointId point, double value);
  [[nodiscard]] foundation::Result<void>
  move_channel_control_point(project::TrackId track, project::TrackControlTarget::Key key,
                             project::AutomationPointId point, std::int64_t zero_based_bar,
                             double value);
  [[nodiscard]] foundation::Result<void>
  remove_channel_control_point(project::TrackId track, project::TrackControlTarget::Key key,
                               project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void>
  remove_channel_control_lane(project::TrackId track, project::TrackControlTarget::Key key);
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_channel_level_automation_point(project::TrackId track, std::int64_t zero_based_bar,
                                     double value);
  [[nodiscard]] foundation::Result<void>
  set_channel_level_automation_value(project::TrackId track, project::AutomationPointId point,
                                     double value);
  // Reposition an existing point on the project's bar grid while retaining its
  // ID, segment shape/handles and lane timebase. One edit is one revision.
  [[nodiscard]] foundation::Result<void>
  move_channel_level_automation_point(project::TrackId track, project::AutomationPointId point,
                                      std::int64_t zero_based_bar, double value);
  [[nodiscard]] foundation::Result<void>
  remove_channel_level_automation_point(project::TrackId track, project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void>
  remove_channel_level_automation_lane(project::TrackId track);
  // A saved VST3 parameter reference supplies the unautomated baseline.
  // The target may be an instrument source or an enabled effect on this track.
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_vst3_parameter_automation_point(project::TrackId track, project::PluginInstanceId plugin,
                                      std::uint32_t parameter, std::int64_t zero_based_bar,
                                      double normalized_value);
  [[nodiscard]] foundation::Result<void>
  set_vst3_parameter_automation_value(project::TrackId track, project::PluginInstanceId plugin,
                                      std::uint32_t parameter, project::AutomationPointId point,
                                      double normalized_value);
  [[nodiscard]] foundation::Result<void>
  remove_vst3_parameter_automation_point(project::TrackId track, project::PluginInstanceId plugin,
                                         std::uint32_t parameter, project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void>
  remove_vst3_parameter_automation_lane(project::TrackId track, project::PluginInstanceId plugin,
                                        std::uint32_t parameter);
  // Send gain uses the send's stable ID and its source track as lane owner.
  // The Quick editor places points at zero-based project bars, preserving an
  // existing elapsed lane's timebase when editing it.
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_send_gain_automation_point(project::SendId send, std::int64_t zero_based_bar, double value);
  [[nodiscard]] foundation::Result<void>
  set_send_gain_automation_value(project::SendId send, project::AutomationPointId point,
                                 double value);
  [[nodiscard]] foundation::Result<void>
  remove_send_gain_automation_point(project::SendId send, project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void> remove_send_gain_automation_lane(project::SendId send);
  // Automation of one built-in effect parameter on a track's chain, with the
  // same bar placement and timebase rules as the synths' (stepped when
  // discrete, ADR-0034).
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_effect_automation_point(project::TrackId track, project::InsertSlotId slot,
                              std::string_view parameter, std::int64_t zero_based_bar,
                              double value);
  [[nodiscard]] foundation::Result<void>
  set_effect_automation_value(project::TrackId track, project::InsertSlotId slot,
                              std::string_view parameter, project::AutomationPointId point,
                              double value);
  [[nodiscard]] foundation::Result<void>
  remove_effect_automation_point(project::TrackId track, project::InsertSlotId slot,
                                 std::string_view parameter, project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void> remove_effect_automation_lane(project::TrackId track,
                                                                       project::InsertSlotId slot,
                                                                       std::string_view parameter);
  // Automation of one continuous built-in synth parameter. Zero-based bar
  // placement uses the project's meter map; a new lane is musical and an
  // existing elapsed lane keeps its timebase.
  [[nodiscard]] foundation::Result<project::AutomationPointId>
  set_native_automation_point(project::TrackId track, std::string_view parameter,
                              std::int64_t zero_based_bar, double value);
  [[nodiscard]] foundation::Result<void>
  set_native_automation_value(project::TrackId track, std::string_view parameter,
                              project::AutomationPointId point, double value);
  [[nodiscard]] foundation::Result<void>
  remove_native_automation_point(project::TrackId track, std::string_view parameter,
                                 project::AutomationPointId point);
  [[nodiscard]] foundation::Result<void> remove_native_automation_lane(project::TrackId track,
                                                                       std::string_view parameter);
};

} // namespace oscillate::application
