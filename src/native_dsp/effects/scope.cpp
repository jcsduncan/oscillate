#include "oscillate/native_dsp/effects/scope.hpp"

#include "dsp/fft.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "scope_presets.hpp"
#include "scope_table.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace oscillate::native_dsp::effects::scope {
namespace {

using scope_table::Channel;
using scope_table::index_of;

namespace p {
constexpr std::size_t sync = index_of("scope.sync");
constexpr std::size_t length = index_of("scope.length");
constexpr std::size_t time = index_of("scope.time");
constexpr std::size_t channel = index_of("scope.channel");
constexpr std::size_t overlay = index_of("scope.overlay");
constexpr std::size_t align = index_of("scope.align");
constexpr std::size_t align_band = index_of("scope.align_band");
} // namespace p

// The alignment window: about 170 ms, a power of two, analysed every half
// window; the low band is below 200 Hz (fourth order, so hats and the
// like fall away fast).
constexpr double align_seconds = 0.17;
constexpr double low_band_hz = 200.0;
// The cross-correlation itself (and the energies) is averaged across
// windows before its peak is read, so windows that catch a whole kick
// outweigh those catching a tail or a gap: about eight windows (0.7 s).
constexpr double correlation_smoothing = 0.25;

[[nodiscard]] int as_int(double value) noexcept { return static_cast<int>(value); }

class ScopeKernel final : public EffectKernel {
public:
  explicit ScopeKernel(double sample_rate) : rate_(sample_rate) {
    window_ = 1;
    while (static_cast<double>(window_) < align_seconds * sample_rate) {
      window_ *= 2;
    }
    fft_ = std::make_unique<native_dsp::dsp::Fft>(2 * window_);
    work_.resize(2 * window_);
    for (auto& ring : rings_) {
      ring.assign(window_, 0.0);
    }
    max_lag_ = static_cast<std::size_t>(std::ceil(max_lag_seconds * sample_rate));
    average_.assign(2 * max_lag_ + 3, 0.0);
    low_pass_ = synth::make_filter_coefficients(synth::FilterMode::lowpass_24,
                                                low_band_hz / sample_rate, 0.0, 1.0);
    reset();
  }

  void control(std::span<const double> v, std::uint32_t,
               const SynthTransport& transport) noexcept override {
    const bool sync = v[p::sync] != 0.0 && transport.beats_per_frame > 0.0;
    const auto length = static_cast<std::size_t>(v[p::length]);
    if (sync) {
      const bool bars = length >= scope_table::first_bar_length;
      const double beats =
          scope_table::length_values[length] * (bars ? transport.beats_per_bar : 1.0);
      // Lock the sweep to the grid at every tick: bar lengths count from bar
      // lines, beat lengths from beats.
      const double cycles = bars ? transport.bar_at_block_start / scope_table::length_values[length]
                                 : transport.beat_at_block_start / beats;
      phase_ = cycles - std::floor(cycles);
      increment_ = transport.beats_per_frame / beats;
      window_ms_ = beats / (transport.beats_per_frame * rate_) * 1e3;
    } else {
      increment_ = 1.0 / std::max(1.0, v[p::time] * 1e-3 * rate_);
      window_ms_ = v[p::time];
    }
    channel_ = as_int(v[p::channel]);
    overlay_ = v[p::overlay] != 0.0;
    align_ = v[p::align] != 0.0;
    low_band_ = as_int(v[p::align_band]) == 1;
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame> sidechain) noexcept override {
    for (std::size_t i = 0; i < audio.size(); ++i) {
      const double l = audio[i].left;
      const double r = audio[i].right;
      double shown = 0.5 * (l + r);
      if (channel_ == Channel::side) {
        shown = 0.5 * (l - r);
      } else if (channel_ == Channel::left) {
        shown = l;
      } else if (channel_ == Channel::right) {
        shown = r;
      }
      const double key = sidechain.empty()
                             ? 0.0
                             : 0.5 * (static_cast<double>(sidechain[i].left) + sidechain[i].right);
      // The sweep.
      const auto column = std::min(columns - 1, static_cast<std::size_t>(phase_ * columns));
      if (column != column_) {
        commit_column();
        column_ = column;
      }
      low_[0] = std::min(low_[0], shown);
      high_[0] = std::max(high_[0], shown);
      low_[1] = std::min(low_[1], key);
      high_[1] = std::max(high_[1], key);
      phase_ += increment_;
      phase_ -= std::floor(phase_);
      // The alignment rings: the track's mid and the sidechain, low-passed
      // alike in the low band so the filter adds no relative delay.
      double a = 0.5 * (l + r);
      double b = key;
      if (low_band_) {
        a = band_[0].process(a, low_pass_);
        b = band_[1].process(b, low_pass_);
      }
      rings_[0][write_ % window_] = a;
      rings_[1][write_ % window_] = b;
      ++write_;
      if (align_ && !sidechain.empty() && write_ % (window_ / 2) == 0 && write_ >= window_) {
        estimate();
      }
    }
  }

  void reset() noexcept override {
    for (auto& ring : rings_) {
      std::fill(ring.begin(), ring.end(), 0.0);
    }
    band_[0].reset();
    band_[1].reset();
    write_ = 0;
    for (auto& set : minimum_) {
      set.fill(0.0F);
    }
    for (auto& set : maximum_) {
      set.fill(0.0F);
    }
    low_ = {1e9, 1e9};
    high_ = {-1e9, -1e9};
    phase_ = 0.0;
    column_ = 0;
    delay_ms_ = 0.0;
    polarity_ = 1.0;
    correlation_ = 0.0;
    std::fill(average_.begin(), average_.end(), 0.0);
    energy_a_ = 0.0;
    energy_b_ = 0.0;
    averaged_ = false;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    for (std::size_t c = 0; c < columns; ++c) {
      out[telemetry::track + 2 * c] = minimum_[0][c];
      out[telemetry::track + 2 * c + 1] = maximum_[0][c];
      out[telemetry::sidechain + 2 * c] = overlay_ ? minimum_[1][c] : 0.0F;
      out[telemetry::sidechain + 2 * c + 1] = overlay_ ? maximum_[1][c] : 0.0F;
    }
    out[telemetry::write_column] = static_cast<float>(column_);
    out[telemetry::delay_ms] = static_cast<float>(delay_ms_);
    out[telemetry::polarity] = static_cast<float>(polarity_);
    out[telemetry::correlation] = static_cast<float>(correlation_);
    out[telemetry::window_ms] = static_cast<float>(window_ms_);
    return telemetry::size;
  }

private:
  void commit_column() noexcept {
    for (std::size_t s = 0; s < 2; ++s) {
      if (low_[s] <= high_[s]) {
        minimum_[s][column_] = static_cast<float>(low_[s]);
        maximum_[s][column_] = static_cast<float>(high_[s]);
      }
    }
    low_ = {1e9, 1e9};
    high_ = {-1e9, -1e9};
  }

  // Cross-correlation by FFT: the two windows packed as a + jb, split,
  // multiplied (a times conj b) and transformed back; the peak within
  // +-max_lag gives the delay (refined by a parabola), its sign the
  // polarity, its size over the energies the correlation.
  void estimate() noexcept {
    const std::size_t n = 2 * window_;
    std::span<std::complex<double>> data(work_.data(), n);
    double energy_a = 0.0;
    double energy_b = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      if (i < window_) {
        const std::size_t at = (write_ + i) % window_; // Oldest first.
        const double a = rings_[0][at];
        const double b = rings_[1][at];
        data[i] = {a, b};
        energy_a += a * a;
        energy_b += b * b;
      } else {
        data[i] = 0.0;
      }
    }
    if (energy_a < 1e-12 || energy_b < 1e-12) {
      return;
    }
    fft_->forward(data);
    // a's and b's spectra from the packed transform, then a conj(b).
    for (std::size_t k = 0; k <= n / 2; ++k) {
      const std::size_t m = (n - k) % n;
      const auto x = data[k];
      const auto y = std::conj(data[m]);
      const auto spectrum_a = 0.5 * (x + y);
      const auto spectrum_b = std::complex<double>(0.0, -0.5) * (x - y);
      const auto cross = spectrum_a * std::conj(spectrum_b);
      data[k] = cross;
      if (m != k) {
        data[m] = std::conj(cross);
      }
    }
    // Inverse by conjugation; r[k] = sum a[i + k] b[i].
    for (auto& value : data) {
      value = std::conj(value);
    }
    fft_->forward(data);
    const auto r = [&](std::ptrdiff_t lag) {
      const auto index = static_cast<std::size_t>(
          (lag % static_cast<std::ptrdiff_t>(n) + static_cast<std::ptrdiff_t>(n)) %
          static_cast<std::ptrdiff_t>(n));
      return data[index].real() / static_cast<double>(n);
    };
    const auto reach = static_cast<std::ptrdiff_t>(max_lag_);
    const double k = averaged_ ? correlation_smoothing : 1.0;
    for (std::ptrdiff_t lag = -reach - 1; lag <= reach + 1; ++lag) {
      auto& value = average_[static_cast<std::size_t>(lag + reach + 1)];
      value += k * (r(lag) - value);
    }
    energy_a_ += k * (energy_a - energy_a_);
    energy_b_ += k * (energy_b - energy_b_);
    averaged_ = true;
    const auto smoothed = [&](std::ptrdiff_t lag) {
      return average_[static_cast<std::size_t>(lag + reach + 1)];
    };
    std::ptrdiff_t best = 0;
    for (std::ptrdiff_t lag = -reach; lag <= reach; ++lag) {
      if (std::fabs(smoothed(lag)) > std::fabs(smoothed(best))) {
        best = lag;
      }
    }
    const double peak = smoothed(best);
    const double sign = peak < 0.0 ? -1.0 : 1.0;
    const double before = sign * smoothed(best - 1);
    const double here = sign * peak;
    const double after = sign * smoothed(best + 1);
    const double curve = before - 2.0 * here + after;
    const double offset = curve < 0.0 ? 0.5 * (before - after) / curve : 0.0;
    delay_ms_ = (static_cast<double>(best) + offset) / rate_ * 1e3;
    polarity_ = sign;
    correlation_ = std::min(1.0, here / std::sqrt(energy_a_ * energy_b_));
  }

  double rate_;
  std::size_t window_ = 0;
  std::unique_ptr<native_dsp::dsp::Fft> fft_;
  std::vector<std::complex<double>> work_;
  std::array<std::vector<double>, 2> rings_;
  std::size_t write_ = 0;
  std::size_t max_lag_ = 0;
  synth::FilterCoefficients low_pass_{};
  std::array<synth::MultimodeFilter, 2> band_{};
  std::array<std::array<float, columns>, 2> minimum_{};
  std::array<std::array<float, columns>, 2> maximum_{};
  std::array<double, 2> low_{};
  std::array<double, 2> high_{};
  double phase_ = 0.0;
  double increment_ = 0.0;
  double window_ms_ = 0.0;
  std::size_t column_ = 0;
  int channel_ = Channel::mid;
  bool overlay_ = true;
  bool align_ = true;
  bool low_band_ = true;
  double delay_ms_ = 0.0;
  double polarity_ = 1.0;
  double correlation_ = 0.0;
  std::vector<double> average_; // Lags -max-1 .. max+1.
  double energy_a_ = 0.0;
  double energy_b_ = 0.0;
  bool averaged_ = false;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<ScopeKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double>, double) { return 1; }
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"scope", "Scope", 1, scope_table::parameters,
                                         scope_presets::presets};
const EffectEngine scope_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, true};

} // namespace

const EffectEngine& engine() { return scope_engine; }

} // namespace oscillate::native_dsp::effects::scope
