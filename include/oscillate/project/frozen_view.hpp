#pragma once

#include "oscillate/project/model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace oscillate::project {

// True when any track plays a freeze render (ADR-0059).
[[nodiscard]] bool has_frozen_track(const Snapshot& snapshot) noexcept;

// A lane that plays after a track's render (ADR-0102): a channel control or
// a send's gain. A render is taken before the channel, so it holds none of
// these, on the track or on its clips.
[[nodiscard]] bool plays_after_render(const AutomationLane& lane) noexcept;

// Where the clips of `track` that carry lanes playing after a render sound,
// in project frames: the span a render must cover so that render_slices can
// place each of them. Empty when no clip carries such a lane.
[[nodiscard]] foundation::Result<std::optional<std::pair<std::uint64_t, std::uint64_t>>>
after_render_clip_span(const Track& track, const timeline::TempoMap& tempo,
                       std::uint32_t sample_rate);

// One clip of a render's audio: render frames `range` at `placement`, with
// the lanes that play after the render of the source clip it stands for.
struct RenderSlice {
  SourceRange range;
  Placement placement;
  std::vector<AutomationLane> automation;
};

// The render of `track`, `frames` long from project frame `first`, as clips
// that play it back exactly through the track's channel (ADR-0102). Each
// clip of `track` with lanes that play after the render becomes a slice at
// that clip's own placement, carrying those lanes, so they take precedence
// over the track's lanes exactly where they did; plain slices fill the gaps.
// Clips wholly outside the render are left out. Fails when such a clip
// crosses the render's edge or overlaps another.
[[nodiscard]] foundation::Result<std::vector<RenderSlice>>
render_slices(const Track& track, const timeline::TempoMap& tempo, std::uint32_t sample_rate,
              std::uint64_t first, std::uint64_t frames);

// The project as it sounds while tracks are frozen (ADR-0059). Each frozen
// track becomes an audio track, with the same id, playing its render from
// first_frame through its fader, pan, mute, solo, sends and routing. Its
// instrument, note effects, inserts, macros, clips and the lanes on them are
// gone, and so are other tracks' sidechain keys into its inserts (the render
// already heard them). The render plays as render_slices, so the channel
// lanes of its clips still apply. Playback and export build from this view,
// so neither runs a frozen track's devices. Empty when no track is frozen.
[[nodiscard]] std::optional<Snapshot> frozen_playback_view(const Snapshot& snapshot);

// frozen_playback_view, or the snapshot itself when nothing is frozen.
[[nodiscard]] Snapshot audible_snapshot(Snapshot snapshot);

// The clip id slice `slice` of a frozen track's render plays under in the
// view: derived from the track's id, so every view of one freeze agrees.
[[nodiscard]] ClipId frozen_render_clip(TrackId track, std::size_t slice = 0) noexcept;

} // namespace oscillate::project
