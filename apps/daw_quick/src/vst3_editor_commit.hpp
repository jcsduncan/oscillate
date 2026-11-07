#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>

namespace oscillate::quick {

// Captured editor state can replace only the unchanged saved target. An
// unrelated project edit may advance the revision while the editor is open.
[[nodiscard]] bool can_commit_vst3_editor_state(const project::Snapshot& snapshot,
                                                project::TrackId track, project::InsertSlotId slot,
                                                const project::Vst3PluginInstance& original);

[[nodiscard]] bool can_commit_vst3_instrument_state(const project::Snapshot& snapshot,
                                                    project::TrackId track,
                                                    const project::Vst3PluginInstance& original);

} // namespace oscillate::quick
