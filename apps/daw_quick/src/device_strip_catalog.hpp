#pragma once

#include "oscillate/native_dsp/parameters.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The horizontal device chain's per-engine strip metadata: the accent colour
// and kind label a strip wears, and the four parameters it puts on knobs.
// Curated ids that a table does not have fall back to the engine's first
// continuous, automatable, live parameters, so a table change never breaks a
// strip.
namespace oscillate::quick {

struct DeviceStripStyle {
  std::string_view engine;
  std::string_view kind;
  std::string_view accent;
  std::array<std::string_view, 4> parameters;
};

// The style for a built-in synth or effect engine id; unknown ids get a
// neutral style with no curated parameters.
[[nodiscard]] const DeviceStripStyle& device_strip_style(std::string_view engine_id) noexcept;

// Up to four parameter ids for the engine's strip, curated first.
[[nodiscard]] std::vector<std::string>
device_strip_parameters(const native_dsp::EngineDescriptor& engine);

} // namespace oscillate::quick
