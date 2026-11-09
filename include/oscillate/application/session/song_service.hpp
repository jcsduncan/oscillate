#pragma once

#include "oscillate/application/arrange_from_loop.hpp"
#include "oscillate/application/session/automation_service.hpp"
#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class SongService : public SessionService {
public:
  SongService(SessionState& state, AutomationService& automation) noexcept
      : SessionService(state), automation_(automation) {}

  // The song's key; none clears it. One undo step; never changes audio.
  [[nodiscard]] foundation::Result<void> set_project_key(std::optional<project::MusicalKey> key);
  // The key most of the project's audio was detected (or corrected) in,
  // counting only sources a clip uses; none without reviewed audio.
  [[nodiscard]] std::optional<project::MusicalKey> suggested_project_key() const;
  // Arrangement markers, one per position; adding at an occupied position
  // renames that marker instead.
  [[nodiscard]] foundation::Result<project::MarkerId> add_marker(timeline::TickPosition position,
                                                                 std::string name = {});
  [[nodiscard]] foundation::Result<void> rename_marker(project::MarkerId marker, std::string name);
  [[nodiscard]] foundation::Result<void> move_marker(project::MarkerId marker,
                                                     timeline::TickPosition position);
  [[nodiscard]] foundation::Result<void> remove_marker(project::MarkerId marker);
  // The first marker after (or the last before) `position`, if any.
  [[nodiscard]] std::optional<project::Marker> next_marker(timeline::TickPosition position) const;
  [[nodiscard]] std::optional<project::Marker>
  previous_marker(timeline::TickPosition position) const;
  // Tempo event positions are project ticks; callers apply the visible snap
  // policy before invoking these undoable commands. Tick zero cannot be
  // removed; removing the last event leaves the one before it holding.
  [[nodiscard]] foundation::Result<void> set_tempo_event(timeline::TickPosition tick, double bpm,
                                                         timeline::TempoShape shape_to_next);
  [[nodiscard]] foundation::Result<void> remove_tempo_event(timeline::TickPosition tick);
  // Moves the tempo event at `from` to `to` with `bpm`, keeping its shape;
  // the event at tick zero only changes its tempo. One undo step.
  [[nodiscard]] foundation::Result<void> move_tempo_event(timeline::TickPosition from,
                                                          timeline::TickPosition to, double bpm);
  // A drawn tempo ramp (ADR-0112), one undo step: from `from` the tempo
  // moves along `shape` to `end_bpm` at `to`, then holds there until the
  // next later event. The tempo playing at `from` is kept and events
  // strictly between are removed.
  [[nodiscard]] foundation::Result<void> set_tempo_ramp(timeline::TickPosition from,
                                                        timeline::TickPosition to, double end_bpm,
                                                        timeline::TempoShape shape);
  // "Ramp to this track's tempo" (plan_transition_ramp) as one undo step.
  [[nodiscard]] foundation::Result<void> ramp_to_clip_tempo(project::ClipId clip);
  [[nodiscard]] foundation::Result<void> set_initial_meter(std::uint8_t numerator,
                                                           std::uint8_t denominator);
  // Adds `bars` bars before bar 1 (ADR-0053), for an intro that starts
  // before the song's first downbeat: everything on the timeline moves later
  // by that span at the opening tempo and meter, and the ruler counts the new
  // bars as 1 - lead .. 0. One undo step.
  [[nodiscard]] foundation::Result<void> insert_lead_bars(std::uint32_t bars);
  // ADR-0080: an Arrange-from-a-loop plan as
  // one undo step. Refused when the project changed since it was planned.
  [[nodiscard]] foundation::Result<void> apply_arrangement(const arrange::Plan& plan);

private:
  [[nodiscard]] std::uint64_t next_marker_number() const;
  // One revision with `events`; the last event's shape becomes a hold.
  [[nodiscard]] foundation::Result<void> replace_tempo(std::vector<timeline::TempoEvent> events);
  AutomationService& automation_;
};

} // namespace oscillate::application
