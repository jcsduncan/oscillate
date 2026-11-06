#pragma once

// Stable identities of project entities.

#include "oscillate/foundation/id.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

struct ProjectTag;
struct TrackTag;
struct ClipTag;
struct NoteTag;
struct SourceTag;
struct PluginInstanceTag;
struct InsertSlotTag;
struct SendTag;
struct AutomationLaneTag;
struct AutomationPointTag;
struct MarkerTag;
using ProjectId = foundation::Id<ProjectTag>;
using TrackId = foundation::Id<TrackTag>;
using ClipId = foundation::Id<ClipTag>;
using NoteId = foundation::Id<NoteTag>;
using SourceId = foundation::Id<SourceTag>;
using PluginInstanceId = foundation::Id<PluginInstanceTag>;
using InsertSlotId = foundation::Id<InsertSlotTag>;
using SendId = foundation::Id<SendTag>;
using AutomationLaneId = foundation::Id<AutomationLaneTag>;
using AutomationPointId = foundation::Id<AutomationPointTag>;
using MarkerId = foundation::Id<MarkerTag>;

} // namespace oscillate::project
