#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/persistence/store.hpp"
#include "oscillate/project/commands.hpp"

#include <functional>
#include <stop_token>
#include <vector>

// Master rebalance (ADR-0123): listen to each audio track for the part it
// plays. Each track is heard from the source its clips play most of; the
// result is the commands that save what was heard, one undo step together.
// Worker thread only.
namespace oscillate::application {

[[nodiscard]] foundation::Result<std::vector<project::Operation>>
listen_for_roles(const persistence::ProjectStore& store, const project::Snapshot& snapshot,
                 const std::function<bool()>& cancelled = {}, const std::stop_token& stop = {});

} // namespace oscillate::application
