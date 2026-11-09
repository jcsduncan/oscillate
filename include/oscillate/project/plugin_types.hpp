#pragma once

// Saved VST3 plugin instances and insert signal paths.

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oscillate::project {

inline constexpr std::size_t max_vst3_plugins_per_track = 32;
inline constexpr std::size_t max_vst3_plugins_per_project = 2'048;
inline constexpr std::size_t max_vst3_state_bytes = std::size_t{4} * 1024 * 1024;
inline constexpr std::size_t max_vst3_project_state_bytes = std::size_t{8} * 1024 * 1024;
inline constexpr std::size_t max_vst3_parameter_references = 256;
inline constexpr std::size_t max_vst3_bundle_path_bytes = 1'024;
inline constexpr std::size_t max_effect_slots_per_path = 64;
inline constexpr std::size_t max_effect_slots_per_project = 4'096;

struct Vst3ParameterReference {
  std::uint32_t id;
  double normalized_value;
  bool operator==(const Vst3ParameterReference&) const = default;
};

// Durable intent only. The bundle path is advisory and is never loaded on restore.
struct Vst3PluginInstance {
  PluginInstanceId id;
  std::string class_id;
  std::string vendor;
  std::string name;
  std::string version;
  std::string bundle_path;
  std::string binary_sha256;
  std::vector<std::uint8_t> component_state;
  std::vector<Vst3ParameterReference> parameters;
  bool missing = false;
  // Independent IEditController state. Nullopt means no saved controller
  // chunk; an empty vector is a saved, intentionally empty chunk.
  std::optional<std::vector<std::uint8_t>> controller_state = std::nullopt;
  bool operator==(const Vst3PluginInstance&) const = default;
};

// Preparatory identity contract for the unified effect chain. A master path
// is a logical project path, never a synthetic TrackId or engine node ID.
struct MasterSignalPath {
  bool operator==(const MasterSignalPath&) const = default;
};
using SignalPathId = std::variant<TrackId, MasterSignalPath>;
enum class NativeInsertKind : std::uint8_t {
  effect = 2 // A built-in effect engine (ADR-0040).
};

// The identity a command gives the effect slot it creates from `serial`, a
// snapshot's next_insert_slot_serial. Commands allocate one serial per new
// slot in operation order, so one apply can name the slots it creates.
[[nodiscard]] foundation::Result<InsertSlotId> command_insert_slot_id(std::uint64_t serial);

} // namespace oscillate::project
