#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace oscillate::native_dsp::synth {

// In-place radix-2 FFT for control-side work (table generation, analysis).
// Forward: X[k] = sum x[n] e^{-i 2 pi k n / N}. Inverse includes the 1/N.
// data.size() must be a power of two.
void fft(std::span<std::complex<double>> data, bool inverse);

inline constexpr std::size_t wavetable_frames = 32;
inline constexpr std::size_t wavetable_levels = 10;
inline constexpr std::size_t wavetable_max_harmonic = 512;
inline constexpr std::size_t wavetable_count = 16;

// One band-limit of every frame: harmonics up to `harmonics`, `length`
// samples per cycle. Frames are stored with one guard sample before and two
// after, so a 4-point cubic read never wraps.
struct WavetableLevel {
  std::uint32_t length = 0;
  std::uint32_t harmonics = 0;
  std::vector<float> data;

  [[nodiscard]] const float* frame(std::size_t index) const noexcept {
    return data.data() + index * (length + 3) + 1;
  }
};

struct Wavetable {
  std::string_view name;
  std::array<WavetableLevel, wavetable_levels> levels;
};

// Level l keeps harmonics up to 512 >> l; the last level is a pure sine.
[[nodiscard]] constexpr std::uint32_t wavetable_level_harmonics(std::size_t level) noexcept {
  return static_cast<std::uint32_t>(wavetable_max_harmonic >> level);
}

// The coarsest level whose top harmonic stays under max_harmonic; the
// oscillator never plays a harmonic above it. Callers pass the highest
// harmonic number that is still below ~0.45 of their run rate.
[[nodiscard]] std::size_t wavetable_level_for(double max_harmonic) noexcept;

// Harmonic amplitudes c[h] (h = 1..512, index 0 unused) of one frame: the
// waveform is sum Re(c[h] e^{i 2 pi h x}). Sine-phase partials use
// c = -i * amplitude.
using WavetableSpectrum = std::array<std::complex<double>, wavetable_max_harmonic + 1>;

// Generators that work in the time domain sample one cycle at this many
// points and convert it with cycle_spectrum.
inline constexpr std::size_t wavetable_cycle_length = 16'384;
void cycle_spectrum(std::span<const double> cycle, WavetableSpectrum& spectrum);

// Builds every band-limited level of one table, calling generate once per
// frame with its morph position t in [0, 1]. Each frame is peak-normalised.
// Control side only (it allocates and runs FFTs).
[[nodiscard]] Wavetable
build_wavetable(std::string_view name,
                const std::function<void(double, WavetableSpectrum&)>& generate);

// Every generated table, built deterministically on the first call (control
// thread, about 11 MB, once per process) and immutable afterwards.
[[nodiscard]] const std::array<Wavetable, wavetable_count>& wavetables();
[[nodiscard]] std::span<const std::string_view> wavetable_names() noexcept;

} // namespace oscillate::native_dsp::synth
