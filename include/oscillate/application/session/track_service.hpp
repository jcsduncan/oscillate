#pragma once

#include "oscillate/application/session/session_service.hpp"

namespace oscillate::application {

class TrackService : public SessionService {
public:
  explicit TrackService(SessionState& state) noexcept : SessionService(state) {}

  // A track's row in the arrangement, or none when it is not in the project.
  [[nodiscard]] std::optional<std::size_t> track_index(project::TrackId track) const;
  [[nodiscard]] foundation::Result<project::TrackId>
  add_instrument_track(std::optional<std::size_t> insert_index = std::nullopt);
  [[nodiscard]] foundation::Result<void> remove_track(project::TrackId track);
  [[nodiscard]] foundation::Result<void> move_track(project::TrackId track, std::size_t to_index);
  // Copies an audio or instrument track to just below itself as one undo
  // step: its instrument, effects, clips, automation, routing, mixer state,
  // name and colour, under fresh identities. Lanes follow the copy's own
  // effect slots, plugins and sends. Buses are not duplicated, since the
  // tracks routed into them would not follow.
  [[nodiscard]] foundation::Result<project::TrackId> duplicate_track(project::TrackId track);
  [[nodiscard]] foundation::Result<void> set_track_muted(project::TrackId track, bool muted);
  [[nodiscard]] foundation::Result<void> set_track_soloed(project::TrackId track, bool soloed);
  [[nodiscard]] foundation::Result<void>
  set_track_level_pan(project::TrackId track, double level, double pan,
                      std::optional<std::uint64_t> gesture = {});
  [[nodiscard]] foundation::Result<void>
  set_master_level(double level, std::optional<std::uint64_t> gesture = {});
  // Control-side routing intent. Playback/export may report this revision as
  // unsupported until their routed graph is active.
  [[nodiscard]] foundation::Result<project::TrackId>
  add_bus_track(project::TrackKind kind, std::optional<std::size_t> insert_index = std::nullopt);
  // The default return tracks a new project starts with, as one undo step.
  [[nodiscard]] foundation::Result<void> seed_default_returns();
  // An empty audio track, ready for clips dragged or imported onto it.
  [[nodiscard]] foundation::Result<project::TrackId>
  add_audio_track(std::optional<std::size_t> insert_index = std::nullopt);

  // Presentation: never audible, each one undo step.
  [[nodiscard]] foundation::Result<void> set_track_name(project::TrackId track, std::string name);
  // An empty colour returns the track (or clip) to the app's palette.
  [[nodiscard]] foundation::Result<void> set_track_color(project::TrackId track,
                                                         std::optional<std::uint32_t> color);
};

} // namespace oscillate::application
