#pragma once

#include "oscillate/project/model.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace oscillate::project {

// Read-only projection of each signal path's durable instrument source and
// ordered effect slots. Routing, export and the Quick bridge build their
// processing chains from it; descriptors do not own a second processing order
// or replace the Snapshot payloads.
struct ProjectedVst3InstrumentSource {
  PluginInstanceId plugin;
  bool missing;
  bool operator==(const ProjectedVst3InstrumentSource&) const = default;
};
struct ProjectedNativeInstrumentSource {
  std::string engine;
  bool operator==(const ProjectedNativeInstrumentSource&) const = default;
};
using ProjectedInstrumentSource =
    std::variant<std::monostate, ProjectedVst3InstrumentSource, ProjectedNativeInstrumentSource>;
struct ProjectedEffectInsert {
  InsertSlotId slot;
  std::variant<PluginInstanceId, NativeInsertKind> identity;
  bool enabled;
  // Native processors carry a generation. VST3 runtime reuse must inspect
  // the full saved plugin identity/state, not this projection.
  std::optional<std::uint64_t> native_settings_generation;
  bool missing_plugin;
  // The built-in effect engine id for NativeInsertKind::effect.
  std::string native_engine{};
  bool operator==(const ProjectedEffectInsert&) const = default;
};
struct SignalPathProjection {
  SignalPathId path;
  std::optional<TrackKind> track_kind; // Empty only for the reserved master path.
  ProjectedInstrumentSource source;
  std::vector<ProjectedEffectInsert> effects;
  bool operator==(const SignalPathProjection&) const = default;
};
struct InsertChainProjection {
  ProjectId project;
  std::uint64_t revision;
  std::vector<SignalPathProjection> paths; // Saved track order, then master.
  bool has_missing_plugins = false;
};

// Validates the complete snapshot before projecting.
[[nodiscard]] foundation::Result<InsertChainProjection>
project_insert_chains(const Snapshot& snapshot);

} // namespace oscillate::project
