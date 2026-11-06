#pragma once

#include "oscillate/native_dsp/synth/runtime.hpp"

#include <span>
#include <string_view>

namespace oscillate::native_dsp::synth {

// Every built-in synth engine. Adding a sibling (a bass or analog-warmth
// engine) means adding it here; the project format stays unchanged.
[[nodiscard]] std::span<const SynthEngine* const> engines() noexcept;
[[nodiscard]] const SynthEngine* find_engine(std::string_view id) noexcept;

// The engine a new instrument track starts with.
[[nodiscard]] const SynthEngine& default_engine() noexcept;

} // namespace oscillate::native_dsp::synth
