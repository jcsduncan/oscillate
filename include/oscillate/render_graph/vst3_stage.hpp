#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"
#include "oscillate/track_chain/track_chain.hpp"

#include <cstddef>
#include <functional>

namespace oscillate::render_graph {

// Prepares one VST3 stage of `snapshot`, an instrument or effect, for blocks
// of at most `max_frames`. The plugin host supplies it (src/application/vst3),
// so neither this module nor export depends on VST3 code.
using Vst3StagePreparer = std::function<foundation::Result<track_chain::Stage>(
    const project::Snapshot& snapshot, const track_chain::SavedStage& stage,
    std::size_t max_frames)>;

} // namespace oscillate::render_graph
