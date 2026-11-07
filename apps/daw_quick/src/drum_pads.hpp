#pragma once

#include "oscillate/native_dsp/synth/knock.hpp"
#include "oscillate/native_dsp/synth/pulse.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oscillate::quick {

// A drum-pad engine plays one sound per key from first_key and ignores
// note-offs: Pulse's twelve pads and Knock's eight. The bridge auditions
// their pads, the piano roll gives each pad a lane and the browser previews
// a beat on them; every one asks here, so a new pad engine is added once.
struct DrumPadLayout {
  std::uint8_t first_key = 0;
  std::size_t pad_count = 0;

  [[nodiscard]] constexpr bool contains(int key) const noexcept {
    return key >= static_cast<int>(first_key) &&
           key < static_cast<int>(first_key) + static_cast<int>(pad_count);
  }
};

[[nodiscard]] inline std::optional<DrumPadLayout> drum_pad_layout(std::string_view engine_id) {
  namespace synth = native_dsp::synth;
  if (engine_id == synth::pulse::engine().descriptor->id)
    return DrumPadLayout{synth::pulse::first_key, synth::pulse::pad_count};
  if (engine_id == synth::knock::engine().descriptor->id)
    return DrumPadLayout{synth::knock::first_key, synth::knock::pad_count};
  return std::nullopt;
}

} // namespace oscillate::quick
