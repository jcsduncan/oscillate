#pragma once

#include <array>
#include <cstddef>

namespace oscillate::native_dsp::synth::nova {

// A formant: its centre and bandwidth in hertz and its level in dB.
struct Formant {
  double hz = 0.0;
  double bandwidth = 0.0;
  double db = 0.0;
};
inline constexpr std::size_t vowel_formants = 5;
using Vowel = std::array<Formant, vowel_formants>;

// The first five formants of a bass voice singing A, E, I, O and U, for the
// vowel filter and the growl wavetables.
inline constexpr std::array<Vowel, 5> vowel_table{{
    {{{600, 60, 0}, {1'040, 70, -7}, {2'250, 110, -9}, {2'450, 120, -9}, {2'750, 130, -20}}},
    {{{400, 40, 0}, {1'620, 80, -12}, {2'400, 100, -9}, {2'800, 120, -12}, {3'100, 120, -18}}},
    {{{250, 60, 0}, {1'750, 90, -30}, {2'600, 100, -16}, {3'050, 120, -22}, {3'340, 120, -28}}},
    {{{400, 40, 0}, {750, 80, -11}, {2'400, 100, -21}, {2'600, 120, -20}, {2'900, 120, -40}}},
    {{{350, 40, 0}, {600, 80, -20}, {2'400, 100, -32}, {2'675, 120, -28}, {2'950, 120, -36}}},
}};

} // namespace oscillate::native_dsp::synth::nova
