#pragma once

#include "oscillate/warp_dsp/real_fft.hpp"

#include <array>
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
// low-passes) sum to a pure delay of half_length frames. Each channel takes
// one forward real transform per hop and each band but the last one product
// and inverse, on the realtime transform's unordered spectra (ADR-0126).
// The last band is the delayed input less the others, so the bands sum to
// the delayed input to double rounding. Output is a further hop late: the
// bank's latency is half_length + hop.
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

  using Buffer = warp_dsp::AlignedBuffer;

  double rate_;
  std::size_t half_;
  std::size_t hop_;
  std::size_t size_;
  warp_dsp::RealFft fft_;
  // Each channel's last size_ frames, its spectrum, and a product.
  std::array<Buffer, 2> history_;
  std::array<Buffer, 2> spectrum_;
  Buffer product_;
  // The low-pass being designed and the one before it (unordered spectra).
  Buffer lowpass_;
  Buffer previous_;
  std::vector<double> taps_;
  // Each band's spectrum but the last's (unordered, scaled by 1 / size_)
  // and every band's output for the hop, per channel.
  std::array<Buffer, max_bands - 1> spectra_{};
  std::array<std::array<std::vector<double>, 2>, max_bands> out_{};
  std::size_t bands_ = 1;
  std::array<double, max_bands - 1> crossovers_{};
  std::size_t position_ = 0;
  bool dirty_ = true;
};

} // namespace oscillate::native_dsp::effects::dsp
