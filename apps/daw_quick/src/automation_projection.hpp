#pragma once

#include "oscillate/project/model.hpp"

#include <QVariantMap>

namespace oscillate::quick::detail {

inline constexpr int visible_bars = 16;

// A clip's level envelope as the arrangement draws it (ClipLevelEnvelope):
// its points as clip fractions, in the lane's own timebase.
QVariantMap clip_channel_automation_projection(const project::Snapshot& snapshot,
                                               const project::Track& track,
                                               const project::Clip& clip);

} // namespace oscillate::quick::detail
