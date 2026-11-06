#pragma once

#include "oscillate/foundation/error.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace oscillate::persistence {

// A user preset for a built-in effect (ADR-0058): the engine, its sparse
// values and its document, in the project codec's own encoding, so a preset
// file and a saved slot say the same thing. Generation and link are a
// slot's, not a preset's: they are neither written nor read.
inline constexpr std::size_t max_effect_preset_bytes = std::size_t{4} * 1024U * 1024U;
[[nodiscard]] std::string encode_native_effect_preset(const project::NativeEffectPatch& patch);
// Structure only; the engine still resolves the values and document.
[[nodiscard]] foundation::Result<project::NativeEffectPatch>
decode_native_effect_preset(std::string_view text);

} // namespace oscillate::persistence
