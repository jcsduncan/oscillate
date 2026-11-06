#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class PedalService : public SessionService {
public:
  explicit PedalService(SessionState& state) noexcept : SessionService(state) {}

  // Pedal lanes (ADR-0035), in clip-local ticks. An empty point list removes
  // the lane.
  [[nodiscard]] foundation::Result<void>
  set_midi_pedal_lane(project::ClipId clip, project::MidiPedal pedal,
                      std::vector<project::MidiPedalPoint> points);
  // Removes every pedal lane from the clip as one undo step.
  [[nodiscard]] foundation::Result<void> clear_midi_pedals(project::ClipId clip);
  // The pedal strip's edits (midi_pedal_edit.hpp), each one undo step:
  // paint a span at a depth (0 lifts), keeping the lane after it; lift the
  // span under a tick; move or resize the span under a tick, keeping its
  // depth; and replace the sustain lane with legato pedalling per chord.
  [[nodiscard]] foundation::Result<void> paint_pedal_span(project::ClipId clip,
                                                          project::MidiPedal pedal,
                                                          timeline::TickPosition start,
                                                          timeline::TickPosition end, double depth);
  [[nodiscard]] foundation::Result<void>
  remove_pedal_span(project::ClipId clip, project::MidiPedal pedal, timeline::TickPosition at);
  [[nodiscard]] foundation::Result<void>
  move_pedal_span(project::ClipId clip, project::MidiPedal pedal, timeline::TickPosition at,
                  timeline::TickPosition start, timeline::TickPosition end);
  [[nodiscard]] foundation::Result<void> pedal_per_chord(project::ClipId clip);
};

} // namespace oscillate::application
