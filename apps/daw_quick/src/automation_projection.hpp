#pragma once

#include "oscillate/project/model.hpp"

#include <QVariantMap>

#include <cstddef>

namespace oscillate::quick::detail {

inline constexpr int visible_bars = 16;
inline constexpr std::size_t max_quick_automation_points = 4;

// Pure read-model views shared by bridge publication and edit previews.
QVariantMap automation_lane_projection(const project::Snapshot& snapshot,
                                       const project::AutomationLane& lane,
                                       std::size_t max_points = max_quick_automation_points,
                                       const project::Placement* placement = nullptr);
QVariantMap channel_automation_projection(const project::Snapshot& snapshot,
                                          const project::Track& track);
QVariantMap clip_channel_automation_projection(const project::Snapshot& snapshot,
                                               const project::Track& track,
                                               const project::Clip& clip);
QVariantMap send_automation_projection(const project::Snapshot& snapshot,
                                       const project::Track& track, project::SendId send);

} // namespace oscillate::quick::detail
