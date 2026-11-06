#include "oscillate/native_dsp/effects/tune.hpp"

#include "dsp/fft.hpp"
#include "oscillate/native_dsp/dsp/math.hpp"
#include "oscillate/native_dsp/synth/filter.hpp"
#include "tune_presets.hpp"
#include "tune_table.hpp"

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

namespace oscillate::native_dsp::effects::tune {
namespace {

using tune_table::index_of;
using tune_table::Mode;

namespace p {
constexpr std::size_t mode = index_of("tune.mode");
constexpr std::size_t reference = index_of("tune.reference");
} // namespace p

// Windows at 48 kHz (scaled to the rate): Instrument 4096 (40 Hz-4 kHz),
// Bass 8192 (20 Hz-1 kHz); analysed every two hops of 1024 frames (about
// 23 readings a second), the hop being the spacing of the two windows
// whose phase refines the pitch.
constexpr std::size_t instrument_window_48k = 4'096;
constexpr std::size_t bass_window_48k = 8'192;
constexpr std::size_t hop_48k = 1'024;
constexpr std::array<std::array<double, 2>, 2> ranges{{{40.0, 4'000.0}, {20.0, 1'000.0}}};
// McLeod: the first maximum within 90% of the highest is the period.
constexpr double peak_threshold = 0.9;
constexpr double min_clarity = 0.8;
constexpr double gate_db = -60.0;
constexpr std::size_t harmonics = 4;
// Kick: the low band below 300 Hz (fourth order); an onset is the band's
// envelope rising 6 dB over its slow average, above -50 dBFS, at least
// 100 ms after the last; a hit is followed for 400 ms or until 40 dB down.
constexpr double kick_band_hz = 300.0;
constexpr double onset_rise = 2.0;
constexpr double onset_floor = 0.003;
constexpr double onset_gap_seconds = 0.1;
constexpr double hit_seconds = 0.4;
constexpr double hit_floor = 0.01; // 40 dB under the hit's peak.
constexpr double trajectory_step_seconds = 0.005;
constexpr std::size_t settled_cycles = 5;

[[nodiscard]] std::size_t scaled(std::size_t frames_48k, double rate) noexcept {
  const double wanted = static_cast<double>(frames_48k) * rate / 48'000.0;
  std::size_t size = 64;
  while (static_cast<double>(size) * 1.41421356 < wanted) {
    size *= 2;
  }
  return size;
}

class TuneKernel final : public EffectKernel {
public:
  explicit TuneKernel(double sample_rate)
      : rate_(sample_rate),
        hop_(static_cast<std::size_t>(std::lround(hop_48k * sample_rate / 48'000.0))) {
    for (const std::size_t w :
         {scaled(instrument_window_48k, sample_rate), scaled(bass_window_48k, sample_rate)}) {
      ffts_.push_back(std::make_unique<native_dsp::dsp::Fft>(2 * w));
      windows_.push_back(w);
    }
    ring_size_ = 1;
    while (ring_size_ < windows_[1] + hop_ + 8) {
      ring_size_ *= 2;
    }
    ring_.assign(ring_size_, 0.0);
    work_.resize(2 * windows_[1]);
    frame_.resize(windows_[1]);
    hann_.resize(windows_[1]);
    nsdf_.assign(windows_[1] + 2, 0.0);
    magnitudes_.assign(windows_[1] + 1, 0.0);
    low_pass_ = synth::make_filter_coefficients(synth::FilterMode::lowpass_24,
                                                kick_band_hz / sample_rate, 0.0, 1.0);
    band_delay_ = 2.613 / (2.0 * std::numbers::pi * kick_band_hz);
    attack_ = 1.0 - std::exp(-1.0 / (0.001 * sample_rate));
    release_ = 1.0 - std::exp(-1.0 / (0.05 * sample_rate));
    slow_ = 1.0 - std::exp(-1.0 / (0.3 * sample_rate));
    reset();
  }

  void control(std::span<const double> v, std::uint32_t, const SynthTransport&) noexcept override {
    mode_ = static_cast<int>(v[p::mode]);
    reference_ = v[p::reference];
  }

  void process(std::span<engine::StereoFrame> audio,
               std::span<const engine::StereoFrame>) noexcept override {
    for (const auto& frame : audio) {
      const double x = 0.5 * (static_cast<double>(frame.left) + frame.right);
      ring_[write_ & (ring_size_ - 1)] = x;
      ++write_;
      if (mode_ == Mode::kick) {
        follow_kick(x);
      }
      if (++since_hop_ == hop_) {
        since_hop_ = 0;
        if (mode_ != Mode::kick && (++hops_ & 1U) == 0) {
          analyse();
        }
        push_history();
      }
    }
  }

  void reset() noexcept override {
    std::fill(ring_.begin(), ring_.end(), 0.0);
    write_ = 0;
    since_hop_ = 0;
    hops_ = 0;
    hz_ = 0.0;
    clarity_ = 0.0;
    level_db_ = -200.0;
    band_.reset();
    envelope_ = 0.0;
    average_ = 0.0;
    previous_ = 0.0;
    last_crossing_ = -1.0;
    tracking_ = false;
    since_onset_ = 0;
    since_hit_ = static_cast<std::uint64_t>(onset_gap_seconds * rate_) + 1;
    trajectory_.fill(0.0F);
    notes_.fill(0.0F);
    notes_write_ = 0;
  }

  [[nodiscard]] std::size_t telemetry(std::span<float> out) const noexcept override {
    if (out.size() < telemetry::size) {
      return 0;
    }
    const auto put = [&](std::size_t at, double value) { out[at] = static_cast<float>(value); };
    put(telemetry::hz, hz_);
    put(telemetry::note, hz_ > 0.0 ? note_of(hz_) : 0.0);
    put(telemetry::cents, hz_ > 0.0 ? cents_of(hz_) : 0.0);
    put(telemetry::clarity, clarity_);
    put(telemetry::level_db, level_db_);
    put(telemetry::settled_hz, settled_hz_);
    put(telemetry::settled_note, settled_hz_ > 0.0 ? note_of(settled_hz_) : 0.0);
    put(telemetry::settled_cents, settled_hz_ > 0.0 ? cents_of(settled_hz_) : 0.0);
    put(telemetry::hits, static_cast<double>(hits_));
    for (std::size_t k = 0; k < trajectory_points; ++k) {
      out[telemetry::trajectory + k] = trajectory_[k];
    }
    for (std::size_t k = 0; k < history; ++k) {
      out[telemetry::notes + k] = notes_[(notes_write_ + k) % history];
    }
    return telemetry::size;
  }

private:
  [[nodiscard]] double note_of(double hz) const noexcept {
    return 69.0 + 12.0 * std::log2(hz / reference_);
  }
  [[nodiscard]] double cents_of(double hz) const noexcept {
    const double note = note_of(hz);
    return 100.0 * (note - std::round(note));
  }

  void push_history() noexcept {
    notes_[notes_write_] = hz_ > 0.0 ? static_cast<float>(note_of(hz_)) : 0.0F;
    notes_write_ = (notes_write_ + 1) % history;
  }

  // McLeod's NSDF by FFT, then the phase refinement.
  void analyse() noexcept {
    const std::size_t which = mode_ == Mode::bass ? 1 : 0;
    const std::size_t w = windows_[which];
    const std::size_t n = 2 * w;
    if (write_ < w + hop_) {
      return;
    }
    // The newest window, oldest sample first.
    double energy = 0.0;
    for (std::size_t i = 0; i < w; ++i) {
      frame_[i] = ring_[(write_ - w + i) & (ring_size_ - 1)];
      energy += frame_[i] * frame_[i];
    }
    level_db_ = energy > 0.0 ? 10.0 * std::log10(2.0 * energy / static_cast<double>(w)) : -200.0;
    if (level_db_ < gate_db) {
      hz_ = 0.0;
      clarity_ = 0.0;
      return;
    }
    std::span<std::complex<double>> data(work_.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      data[i] = i < w ? frame_[i] : 0.0;
    }
    ffts_[which]->forward(data);
    // Keep the spectrum's magnitudes: they choose the harmonic to refine.
    spectrum_bins_ = n;
    for (std::size_t k = 0; k <= n / 2; ++k) {
      magnitudes_[k] = std::abs(data[k]);
    }
    for (auto& value : data) {
      value = std::norm(value);
    }
    ffts_[which]->forward(data); // Real and even: forward equals inverse up to scale.
    const double scale = 1.0 / static_cast<double>(n);
    const auto [low_hz, high_hz] = ranges[which];
    const auto min_lag = static_cast<std::size_t>(std::floor(rate_ / high_hz));
    const auto max_lag = std::min(w - 2, static_cast<std::size_t>(std::ceil(rate_ / low_hz)));
    // m(tau) = sum over the overlap of x_j^2 + x_{j+tau}^2.
    double m = 2.0 * energy;
    for (std::size_t tau = 0; tau <= max_lag + 1; ++tau) {
      if (tau > 0) {
        m -= frame_[tau - 1] * frame_[tau - 1] + frame_[w - tau] * frame_[w - tau];
      }
      nsdf_[tau] = m > 0.0 ? 2.0 * data[tau].real() * scale / m : 0.0;
    }
    // Key maxima between positive-going and negative-going zero crossings.
    double highest = 0.0;
    std::size_t key_count = 0;
    std::array<std::size_t, 64> keys{};
    std::size_t tau = 1;
    while (tau <= max_lag && nsdf_[tau] > 0.0) {
      ++tau; // Past the lobe around zero lag.
    }
    while (tau <= max_lag && key_count < keys.size()) {
      while (tau <= max_lag && nsdf_[tau] <= 0.0) {
        ++tau;
      }
      std::size_t best = tau;
      while (tau <= max_lag && nsdf_[tau] > 0.0) {
        if (nsdf_[tau] > nsdf_[best]) {
          best = tau;
        }
        ++tau;
      }
      if (best <= max_lag && best >= min_lag && nsdf_[best] > 0.0) {
        keys[key_count++] = best;
        highest = std::max(highest, nsdf_[best]);
      }
    }
    std::size_t chosen = 0;
    for (std::size_t k = 0; k < key_count; ++k) {
      if (nsdf_[keys[k]] >= peak_threshold * highest) {
        chosen = keys[k];
        break;
      }
    }
    if (chosen == 0 || highest < min_clarity) {
      hz_ = 0.0;
      clarity_ = highest;
      return;
    }
    const double before = nsdf_[chosen - 1];
    const double here = nsdf_[chosen];
    const double after = nsdf_[chosen + 1];
    const double curve = before - 2.0 * here + after;
    const double offset = curve < 0.0 ? 0.5 * (before - after) / curve : 0.0;
    clarity_ = here;
    hz_ = refine(rate_ / (static_cast<double>(chosen) + offset), w);
  }

  // The strongest of the first four harmonics, measured on two Hann
  // windows a hop apart: for a steady tone its phase advances by exactly
  // 2 pi f hop / fs, which pins f far finer than the lag.
  [[nodiscard]] double refine(double coarse, std::size_t w) noexcept {
    if (hann_size_ != w) {
      for (std::size_t i = 0; i < w; ++i) {
        hann_[i] = oscillate::native_dsp::dsp::hann(static_cast<double>(i), static_cast<double>(w));
      }
      hann_size_ = w;
    }
    const auto dft = [&](double hz, std::size_t end) {
      const double step = 2.0 * std::numbers::pi * hz / rate_;
      std::complex<double> rotor{1.0, 0.0};
      const std::complex<double> turn{std::cos(step), -std::sin(step)};
      std::complex<double> sum{0.0, 0.0};
      for (std::size_t i = 0; i < w; ++i) {
        sum += ring_[(end - w + i) & (ring_size_ - 1)] * hann_[i] * rotor;
        rotor *= turn;
        if ((i & 255U) == 255U) {
          rotor /= std::abs(rotor);
        }
      }
      return sum;
    };
    // The strongest of the first four harmonics, from the NSDF's own
    // spectrum (its zero padding doubles the bins).
    double best_magnitude = 0.0;
    std::size_t best = 1;
    const double bin_hz = rate_ / static_cast<double>(spectrum_bins_);
    for (std::size_t h = 1; h <= harmonics; ++h) {
      const double hz = coarse * static_cast<double>(h);
      if (hz > native_dsp::dsp::nyquist_guard * rate_) {
        break;
      }
      const auto bin = static_cast<std::size_t>(std::lround(hz / bin_hz));
      double magnitude = 0.0;
      for (std::size_t k = bin > 1 ? bin - 1 : 0; k <= std::min(spectrum_bins_ / 2, bin + 1); ++k) {
        magnitude = std::max(magnitude, magnitudes_[k]);
      }
      if (magnitude > best_magnitude) {
        best_magnitude = magnitude;
        best = h;
      }
    }
    const double analysed = coarse * static_cast<double>(best);
    const auto now = dft(analysed, write_);
    const auto then = dft(analysed, write_ - hop_);
    const double advance = std::arg(now * std::conj(then));
    const double expected = 2.0 * std::numbers::pi * analysed * static_cast<double>(hop_) / rate_;
    double deviation = advance - expected;
    deviation -= 2.0 * std::numbers::pi * std::round(deviation / (2.0 * std::numbers::pi));
    const double refined =
        analysed + deviation * rate_ / (2.0 * std::numbers::pi * static_cast<double>(hop_));
    return refined / static_cast<double>(best);
  }

  // Kick mode, every sample.
  void follow_kick(double x) noexcept {
    const double y = band_.process(x, low_pass_);
    const double level = std::fabs(y);
    envelope_ += (level > envelope_ ? attack_ : release_) * (level - envelope_);
    average_ += slow_ * (envelope_ - average_);
    ++since_hit_;
    if (!tracking_ && envelope_ > onset_floor && envelope_ > onset_rise * average_ &&
        since_hit_ > static_cast<std::uint64_t>(onset_gap_seconds * rate_)) {
      tracking_ = true;
      since_onset_ = 0;
      since_hit_ = 0;
      hit_peak_ = envelope_;
      filled_ = 0;
      cycles_ = 0;
      last_crossing_ = -1.0;
      trajectory_.fill(0.0F);
      ++hits_;
    }
    if (tracking_) {
      hit_peak_ = std::max(hit_peak_, envelope_);
      // A positive-going zero crossing, interpolated.
      if (previous_ < 0.0 && y >= 0.0) {
        const double at = static_cast<double>(since_onset_) - y / (y - previous_);
        if (last_crossing_ >= 0.0) {
          // Each cycle's frequency belongs at its middle, less the low-pass's
          // group delay; the grid between two cycles is interpolated.
          const double hz = rate_ / (at - last_crossing_);
          const double middle = 0.5 * (at + last_crossing_) / rate_ - band_delay_;
          hz_ = hz;
          recent_[cycles_ % settled_cycles] = hz;
          ++cycles_;
          const double from_time = cycles_ > 1 ? previous_middle_ : 0.0;
          const double from_hz = cycles_ > 1 ? previous_hz_ : hz;
          while (filled_ < trajectory_points) {
            const double t = static_cast<double>(filled_) * trajectory_step_seconds;
            if (t > middle) {
              break;
            }
            const double w = middle > from_time
                                 ? std::clamp((t - from_time) / (middle - from_time), 0.0, 1.0)
                                 : 1.0;
            trajectory_[filled_++] = static_cast<float>(from_hz + (hz - from_hz) * w);
          }
          previous_middle_ = middle;
          previous_hz_ = hz;
        }
        last_crossing_ = at;
      }
      ++since_onset_;
      const double elapsed = static_cast<double>(since_onset_) / rate_;
      if (elapsed >= hit_seconds || envelope_ < hit_floor * hit_peak_) {
        finish_hit();
      }
    }
    previous_ = y;
  }

  // The settled pitch: the median of the last cycles measured.
  void finish_hit() noexcept {
    tracking_ = false;
    const std::size_t count = std::min(cycles_, settled_cycles);
    if (count == 0) {
      return;
    }
    std::array<double, settled_cycles> last{};
    for (std::size_t k = 0; k < count; ++k) {
      last[k] = recent_[(cycles_ - 1 - k) % settled_cycles];
    }
    // Insertion sort: at most five values.
    for (std::size_t i = 1; i < count; ++i) {
      for (std::size_t j = i; j > 0 && last[j - 1] > last[j]; --j) {
        std::swap(last[j - 1], last[j]);
      }
    }
    settled_hz_ = last[count / 2];
  }

  double rate_;
  std::size_t hop_;
  std::vector<std::unique_ptr<native_dsp::dsp::Fft>> ffts_;
  std::vector<std::size_t> windows_;
  std::vector<double> ring_;
  std::size_t ring_size_ = 0;
  std::vector<std::complex<double>> work_;
  std::vector<double> frame_;
  std::vector<double> hann_;
  std::size_t hann_size_ = 0;
  std::vector<double> nsdf_;
  std::vector<double> magnitudes_;
  std::size_t spectrum_bins_ = 2;
  std::size_t write_ = 0;
  std::size_t since_hop_ = 0;
  std::uint64_t hops_ = 0;
  int mode_ = Mode::instrument;
  double reference_ = 440.0;
  double hz_ = 0.0;
  double clarity_ = 0.0;
  double level_db_ = -200.0;
  // Kick.
  synth::FilterCoefficients low_pass_{};
  synth::MultimodeFilter band_;
  double attack_ = 1.0;
  double release_ = 1.0;
  double slow_ = 1.0;
  double envelope_ = 0.0;
  double average_ = 0.0;
  double previous_ = 0.0;
  double last_crossing_ = -1.0;
  double hit_peak_ = 0.0;
  double previous_middle_ = 0.0;
  double previous_hz_ = 0.0;
  // A fourth-order Butterworth low-pass delays low tones by 2.613 / wc.
  double band_delay_ = 0.0;
  bool tracking_ = false;
  std::uint64_t since_onset_ = 0;
  std::uint64_t since_hit_ = 0;
  std::size_t filled_ = 0;
  std::size_t cycles_ = 0;
  std::array<double, settled_cycles> recent_{};
  double settled_hz_ = 0.0;
  std::uint64_t hits_ = 0;
  std::array<float, trajectory_points> trajectory_{};
  std::array<float, history> notes_{};
  std::size_t notes_write_ = 0;
};

std::unique_ptr<EffectKernel> create_kernel(std::span<const double>, double sample_rate) {
  return std::make_unique<TuneKernel>(sample_rate);
}

std::uint64_t tail_frames(std::span<const double>, double) { return 1; }
std::uint32_t latency_frames(std::span<const double>, double) { return 0; }

const synth::EngineDescriptor descriptor{"tune", "Tune", 1, tune_table::parameters,
                                         tune_presets::presets};
const EffectEngine tune_engine{&descriptor, &create_kernel, &tail_frames, &latency_frames, false};

} // namespace

const EffectEngine& engine() { return tune_engine; }

} // namespace oscillate::native_dsp::effects::tune
