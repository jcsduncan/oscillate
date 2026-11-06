#pragma once

#include "oscillate/native_dsp/synth/parameters.hpp"
#include "oscillate/project/model.hpp"

#include <cstddef>
#include <optional>
#include <span>

// Nova's factory presets: flat values plus each preset's document (routes,
// shapes, effects, media, macro names).
namespace oscillate::native_dsp::synth::nova {

[[nodiscard]] std::span<const SynthPreset> factory_presets() noexcept;
// The document for a preset, or for the defaults (nullopt).
[[nodiscard]] project::NativeSynthDocument preset_document(std::optional<std::size_t> preset);

} // namespace oscillate::native_dsp::synth::nova
