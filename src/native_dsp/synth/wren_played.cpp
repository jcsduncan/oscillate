#include "oscillate/native_dsp/synth/wren.hpp"
#include "oscillate/native_dsp/synth/wren_breath.hpp"
#include "wren_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// wren::played: the rules the kernel applies to Breath's decision as a note
// starts, kept pure so previews show exactly what playback does.
namespace oscillate::native_dsp::synth::wren {
namespace {

namespace wt = wren_table;
namespace b = breath;

constexpr std::size_t articulation_index = wt::index_of("wren.articulation");
constexpr std::size_t voices_index = wt::index_of("wren.voices");
constexpr std::size_t vibrato_index = wt::index_of("wren.breath.vibrato");

[[nodiscard]] std::size_t choice_of(double value, std::size_t count) noexcept {
  const auto index = static_cast<long long>(std::lround(value));
  return static_cast<std::size_t>(
      std::clamp<long long>(index, 0, static_cast<long long>(count) - 1));
}

} // namespace

Played played(std::span<const double> values, int key, double length_beats,
              const b::Decision& decision) noexcept {
  Played out;
  out.tongue = decision.tongue;
  out.length_beats = length_beats;
  out.vibrato = decision.vibrato;
  out.grace = decision.grace && key + decision.grace_step <= last_key;
  if (values.size() != wt::parameters.size()) {
    return out;
  }
  const bool poly =
      choice_of(values[voices_index], wt::voice_modes.size()) == static_cast<std::size_t>(wt::poly);
  if (poly && out.tongue == b::Tongue::slur) {
    out.tongue = b::Tongue::t;
  }
  const bool staccato = choice_of(values[articulation_index], wt::articulations.size()) ==
                        static_cast<std::size_t>(b::Articulation::staccato);
  if (staccato && (length_beats <= 0.0 || length_beats > b::staccato_beats)) {
    out.length_beats = b::staccato_beats;
  }
  out.vibrato = decision.vibrato * values[vibrato_index];
  return out;
}

} // namespace oscillate::native_dsp::synth::wren
