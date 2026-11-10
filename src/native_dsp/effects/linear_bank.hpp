#pragma once

#include "dsp/fft.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The linear-phase crossover bank shared by the multiband effects (Strata,
// ADR-0082; Strike, ADR-0118).
namespace oscillate::native_dsp::effects::dsp {

// The bank's sizes at a rate: at 48 kHz a 2047-tap Blackman-windowed sinc
// per crossover, run by overlap-save on 512-frame hops in 4096-point FFTs,
// all doubled per doubling of the rate.
struct BankFrames {
  std::size_t half_length = 0; // FIR taps either side of its centre
  std::size_t hop = 0;         // the FFT block
  std::size_t fft = 0;
  // half_length + hop: the bank's output is that late.
  [[nodiscard]] std::uint32_t latency() const noexcept {
    return static_cast<std::uint32_t>(half_length + hop);
  }
};
[[nodiscard]] BankFrames bank_frames(double sample_rate) noexcept;

// Splits stereo into bands whose FIRs (differences of Blackman-windowed sinc
// low-passes) sum to a pure delay of half_length frames. Left and right run
// as one complex signal through one FFT per hop, and every band's spectrum
// is real-symmetric, so each band needs one inverse transform. Output is a
// further hop late: the bank's latency is half_length + hop.
class LinearBank {
public:
  static constexpr std::size_t max_bands = 6;

  // Control only.
  LinearBank(const BankFrames& frames, double sample_rate);

  // `crossovers` holds at least bands - 1 ascending frequencies; a change
  // designs the filters again at the next hop.
  void set(std::size_t bands, std::span<const double> crossovers) noexcept;
  void reset() noexcept;
  [[nodiscard]] std::size_t bands() const noexcept { return bands_; }

  // Writes bands() pairs (the bank's output now) and takes one input frame.
  void process(double left, double right, std::span<double> out_left,
               std::span<double> out_right) noexcept;

private:
  void run() noexcept;
  void design() noexcept;

  double rate_;
  std::size_t half_;
  std::size_t hop_;
  std::size_t size_;
  native_dsp::dsp::Fft fft_;
  std::vector<std::complex<double>> history_;
  std::vector<std::complex<double>> work_;
  std::vector<std::complex<double>> lowpass_;
  std::vector<std::complex<double>> previous_;
  std::vector<double> taps_;
  std::array<std::vector<std::complex<double>>, max_bands> spectra_{};
  std::array<std::vector<std::complex<double>>, max_bands> out_{};
  std::size_t bands_ = 1;
  std::array<double, max_bands - 1> crossovers_{};
  std::size_t position_ = 0;
  bool dirty_ = true;
};

} // namespace oscillate::native_dsp::effects::dsp
