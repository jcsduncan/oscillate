#pragma once

#include "dsp/crossover.hpp"
#include "oscillate/engine/graph.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/effects/reduction_bands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace oscillate::native_dsp::effects::dsp {

// Where a dynamics processor's gain reduction comes from
// (reduction_bands.hpp): the detector's mid signal, (L + R) / 2, split by a
// chain of fourth-order Linkwitz-Riley crossovers at the inner band edges.
// Only band power is wanted, so the bands skip the tree's phase
// compensation: seven splits, 28 biquad sections a sample. Each block's
// power per band, weighted by the detector's own filter, becomes the bands'
// shares while the block's reduction is active; the shares glide to them
// (or to 0 once reduction stops) with the display smoothing.
class ReductionBands {
public:
  static constexpr std::size_t bands = reduction_bands::count;
  static constexpr std::size_t splits = bands - 1;

  // Control only.
  void prepare(double sample_rate) noexcept {
    const double ceiling_hz = native_dsp::dsp::nyquist_guard * sample_rate;
    for (std::size_t k = 0; k < splits; ++k) {
      const double hz = std::min(reduction_bands::edge_hz(k + 1), ceiling_hz);
      split_[k].set(native_dsp::dsp::Lr4Design::at(hz, sample_rate));
    }
    keep_per_frame_ = std::exp(-1.0 / (reduction_bands::smoothing_seconds * sample_rate));
    weights_.fill(1.0);
    reset();
  }

  // Control or callback: how much of each band's power reaches the
  // detector (its own filter's power gain at the band centre); 1 when none.
  void set_weights(const std::array<double, bands>& weights) noexcept { weights_ = weights; }

  void reset() noexcept {
    for (auto& split : split_) {
      split.reset();
    }
    power_.fill(0.0);
    shares_.fill(0.0);
  }

  // Callback: one block of the detector signal.
  void analyse(std::span<const engine::StereoFrame> detector) noexcept {
    for (const auto& frame : detector) {
      double rest =
          mid_weight * (static_cast<double>(frame.left) + static_cast<double>(frame.right));
      if (!std::isfinite(rest)) {
        rest = 0.0;
      }
      for (std::size_t k = 0; k < splits; ++k) {
        const auto split = split_[k].process(rest);
        power_[k] += split.low * split.low;
        rest = split.high;
      }
      power_[splits] += rest * rest;
    }
  }

  // Callback, after analyse: the reduction over that block (dB, positive)
  // and its length.
  // The order (reduction, then frames) is the call contract.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void settle(double reduction_db, std::size_t frames) noexcept {
    double total = 0.0;
    for (std::size_t band = 0; band < bands; ++band) {
      total += power_[band] * weights_[band];
    }
    const bool active = reduction_db >= reduction_bands::active_db && total > 0.0;
    const double approach = 1.0 - std::pow(keep_per_frame_, static_cast<double>(frames));
    for (std::size_t band = 0; band < bands; ++band) {
      const double target = active ? power_[band] * weights_[band] / total : 0.0;
      shares_[band] += (target - shares_[band]) * approach;
    }
    power_.fill(0.0);
  }

  // Writes `bands` shares, low to high.
  void write(std::span<float> out) const noexcept {
    for (std::size_t band = 0; band < bands; ++band) {
      out[band] = static_cast<float>(shares_[band]);
    }
  }

private:
  static constexpr double mid_weight = 0.5;
  std::array<native_dsp::dsp::Lr4Split, splits> split_{};
  std::array<double, bands> weights_{};
  std::array<double, bands> power_{};
  std::array<double, bands> shares_{};
  double keep_per_frame_ = 0.0;
};

} // namespace oscillate::native_dsp::effects::dsp
