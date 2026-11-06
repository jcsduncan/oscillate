#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class RoutingService : public SessionService {
public:
  explicit RoutingService(SessionState& state) noexcept : SessionService(state) {}

  [[nodiscard]] foundation::Result<void>
  set_main_output(project::TrackId track, std::optional<project::TrackId> destination);
  // Sends `tracks` to a group bus as one undo step: `bus`, or, without one, a
  // new group bus just above the first of them in the track order, named
  // "Drums" when every track reads as a drum (kick, snare, hats, perc…),
  // otherwise "Bus N". Buses among `tracks` and `bus` itself are left as
  // they are. Returns the bus.
  [[nodiscard]] foundation::Result<project::TrackId>
  route_tracks_to_bus(std::span<const project::TrackId> tracks,
                      std::optional<project::TrackId> bus = std::nullopt);
  // Sends `tracks` straight to the master again, as one undo step.
  [[nodiscard]] foundation::Result<void>
  route_tracks_to_master(std::span<const project::TrackId> tracks);
  [[nodiscard]] foundation::Result<project::SendId>
  add_send(project::TrackId track, project::TrackId destination, double gain);
  [[nodiscard]] foundation::Result<project::SendId> add_sidechain(project::TrackId track,
                                                                  project::TrackId destination,
                                                                  project::InsertSlotId compressor,
                                                                  double gain);
  // The track keying one effect's sidechain input, set from the effect's own
  // panel in one undoable step: an empty source removes the key, another
  // track replaces the current one, and the same track only changes its gain.
  [[nodiscard]] foundation::Result<void>
  set_sidechain_source(project::TrackId destination, project::InsertSlotId effect,
                       std::optional<project::TrackId> source, double gain);
  [[nodiscard]] foundation::Result<void> set_send_gain(project::SendId send, double gain);
  [[nodiscard]] foundation::Result<void> remove_send(project::SendId send);
  // Every new audio, instrument or group track gets a silent (gain 0)
  // audible send to each return track, and a new return gets one from every
  // other track, in the same undo step, so send knobs are always there.
  // This sets the level of `track`'s audible send to `return_track`, adding
  // the send if it is missing.
  [[nodiscard]] foundation::Result<void> set_send_level(project::TrackId track,
                                                        project::TrackId return_track, double gain,
                                                        std::optional<std::uint64_t> gesture = {});
};

} // namespace oscillate::application
