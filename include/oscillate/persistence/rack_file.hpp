#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oscillate::persistence {

// A rack (ADR-0059): a track's built-in device set saved as a user file, the
// note effects, the built-in instrument, the built-in effects and the macros
// with their mappings, documents included, in the project codec's own
// encoding. Slots carry the rack's own ids (rack_slot_id(1), (2)...) and an
// instrument mapping names rack_track_id(); loading allocates real ones.
// Effect links and generations are a slot's, not a rack's: neither is kept.
inline constexpr std::size_t max_rack_bytes = std::size_t{8} * 1024U * 1024U;

struct TrackRack {
  std::optional<project::NativeInstrumentPatch> instrument;
  std::vector<project::NoteEffectInsert> note_effects;
  // Built-in effects only (NativeEffectPatch processors).
  std::vector<project::EffectInsert> effects;
  std::vector<project::Macro> macros;
  bool operator==(const TrackRack&) const = default;
};

[[nodiscard]] project::TrackId rack_track_id();
// The rack's slot ids count from 1.
[[nodiscard]] project::InsertSlotId rack_slot_id(std::size_t ordinal);

// Structure only: limits, unique rack slot ids, built-in effects, and macro
// destinations that name the rack's own instrument or slots. Engines still
// resolve the values and documents when the rack is loaded.
[[nodiscard]] foundation::Result<void> validate_rack(const TrackRack& rack);
[[nodiscard]] std::string encode_rack(const TrackRack& rack);
[[nodiscard]] foundation::Result<TrackRack> decode_rack(std::string_view text);

} // namespace oscillate::persistence
