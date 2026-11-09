#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/routing/project_route_plan.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <span>

// What a saved edit changed, as routed playback decides how to apply it:
// the comparisons the control's revision, live-edit, graph and source-edit
// paths share.
namespace oscillate::application::routed_playback {

[[nodiscard]] inline foundation::Result<void> rejected(foundation::ErrorCode code,
                                                       const char* message) {
  return foundation::Result<void>::failure({code, message});
}

[[nodiscard]] bool same_sources(std::span<const routing::ProjectRouteSource> left,
                                std::span<const routing::ProjectRouteSource> right);

[[nodiscard]] bool same_timing(const project::Snapshot& left, const project::Snapshot& right);

// Two versions of a built-in instrument track that differ only in what its
// player renders from a plan: its MIDI clips (notes, pedals, placement and
// their lanes), its player lanes, note effect values and switches, and its
// macros' values. Note effect layout is checked against the running player.
[[nodiscard]] bool player_edit_only(const project::Track& before, const project::Track& after);

// A VST3 instrument track whose MIDI notes, pedals or clip placement alone
// changed, with no lane on any of its clips: the running plugin takes the
// new MIDI as a plan at its next block (ADR-0101).
[[nodiscard]] bool vst3_midi_edit_only(const project::Track& before, const project::Track& after);

[[nodiscard]] bool same_audio_content(const project::Snapshot& prepared,
                                      const project::Snapshot& candidate);

// An edit only a whole replacement carries (ADR-0098): it adds or removes
// tracks, changes the tempo or meter, the source media or an instrument, or
// the MIDI of a built-in instrument's clips with a lane for something else.
// The replacement source and graph switch in together at a primed block.
[[nodiscard]] bool needs_whole_switch(const project::Snapshot& published,
                                      const project::Snapshot& candidate);

[[nodiscard]] bool audio_clip_edit_only(const project::Snapshot& before,
                                        const project::Snapshot& after);

// True when `chain` runs exactly the `saved` stages.
[[nodiscard]] bool runs(const track_chain::PreparedChain& chain,
                        std::span<const track_chain::SavedStage> saved);

} // namespace oscillate::application::routed_playback
