#pragma once

#include "model_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

// The validation families validate() composes: automation, macros and note
// effects, and audio and MIDI clips.
namespace oscillate::project::detail {

struct RenderedInterval {
  std::int64_t start;
  std::int64_t end;
  ClipId id;
};

[[nodiscard]] Result<void> validate_automation_lanes(const Track& track,
                                                     const std::vector<AutomationLane>& lanes,
                                                     std::size_t owner_limit, bool clip_owned,
                                                     std::set<AutomationLaneId>& lane_ids,
                                                     std::set<AutomationPointId>& point_ids,
                                                     std::size_t& point_count);

// Structure only (ADR-0059); the engine resolves ids and ranges.
[[nodiscard]] Result<void> validate_note_effect(const NoteEffectInsert& effect);

// A macro's mappings must name parameters on its own track, each mapped by
// at most one macro, with a sound copy of the parameter's domain.
[[nodiscard]] Result<void> validate_macros(const Track& track);

[[nodiscard]] Result<void> validate_frozen(const Track& track, const Snapshot& snapshot);

// Every role's gain and EQ within its range (ADR-0123).
[[nodiscard]] Result<void> validate_rebalance(const MasterRebalance& rebalance);

[[nodiscard]] Result<RenderedInterval> render_placement(const Snapshot& state,
                                                        const Placement& placement, ClipId id);

[[nodiscard]] Result<RenderedInterval> render_clip(const Snapshot& state, const Clip& clip);

// Structure and bounds only; whether the ids exist in the analysed take is
// checked when the take is rendered (ADR-0081).
[[nodiscard]] Result<void> validate_clay(const ClayTake& take);

[[nodiscard]] Result<void> validate_clip_processing(const Snapshot& state, const Clip& clip,
                                                    const RenderedInterval& rendered);

[[nodiscard]] Result<void> validate_midi_timing(const Snapshot& state, const MidiClip& clip,
                                                const RenderedInterval& rendered,
                                                std::size_t& event_count);

[[nodiscard]] Result<void> validate_midi_pedals(const MidiClip& clip);

} // namespace oscillate::project::detail
