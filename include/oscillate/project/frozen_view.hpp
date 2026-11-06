#pragma once

#include "oscillate/project/model.hpp"

#include <optional>

namespace oscillate::project {

// True when any track plays a freeze render (ADR-0059).
[[nodiscard]] bool has_frozen_track(const Snapshot& snapshot) noexcept;

// The project as it sounds while tracks are frozen (ADR-0059). Each frozen
// track becomes an audio track, with the same id, playing its render from
// first_frame through its fader, pan, mute, solo, sends and routing. Its
// instrument, note effects, inserts, macros, clips and the lanes on them are
// gone, and so are other tracks' sidechain keys into its inserts (the render
// already heard them). Playback and export build from this view, so neither
// runs a frozen track's devices. Empty when no track is frozen.
[[nodiscard]] std::optional<Snapshot> frozen_playback_view(const Snapshot& snapshot);

// frozen_playback_view, or the snapshot itself when nothing is frozen.
[[nodiscard]] Snapshot audible_snapshot(Snapshot snapshot);

// The clip id a frozen track's render plays under in the view: derived from
// the track's id, so every view of one freeze agrees.
[[nodiscard]] ClipId frozen_render_clip(TrackId track) noexcept;

} // namespace oscillate::project
